import csv
import io
import sqlite3
import uuid
from datetime import datetime, timedelta, timezone

from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import Response

from .database import db_cursor, utc_now_iso
from .schemas import HeartbeatIn, ManualRunIn, RunBatchIn, RunIn, RunUpdate

router = APIRouter(prefix="/api/v1/runs", tags=["runs"])

# Älter als das gilt age_ms als unplausibel (z.B. fehlerhafte Uhr) -> Eingangszeit verwenden
MAX_RUN_AGE = timedelta(days=7)


def _current_active_person(cur):
    row = cur.execute("SELECT value FROM app_state WHERE key='active_person_id'").fetchone()
    try:
        pid = int(row[0]) if row else 1
    except (TypeError, ValueError):
        pid = 1
    person = cur.execute("SELECT id, name FROM persons WHERE id=?", (pid,)).fetchone()
    return (1, "Unbekannt") if not person else (person["id"], person["name"])


def _person_at(cur, at_iso: str):
    """Aktive Person zum Zeitpunkt at_iso. Läufe aus der Warteschlange der Waage kommen verspätet an;
    sie gehören der Person, die zur MESSZEIT aktiv war, nicht der beim Eintreffen aktiven."""
    row = cur.execute(
        """SELECT p.id, p.name FROM person_activations a
           JOIN persons p ON p.id = a.person_id
           WHERE a.activated_at <= ?
           ORDER BY a.activated_at DESC, a.id DESC LIMIT 1""",
        (at_iso,),
    ).fetchone()
    if row:
        return row["id"], row["name"]
    return _current_active_person(cur)


def _measured_at(received: datetime, age_ms: int | None) -> datetime:
    if age_ms is None:
        return received
    measured = received - timedelta(milliseconds=age_ms)
    return measured if received - measured <= MAX_RUN_AGE else received


def _set_state(cur, key: str, value: str) -> None:
    cur.execute("INSERT INTO app_state(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (key, value))


def _insert_run(cur, run: RunIn):
    received = datetime.now(timezone.utc)
    received_at = received.isoformat()
    measured_at = _measured_at(received, run.age_ms).isoformat()
    person_id, person_name = _person_at(cur, measured_at)
    duplicate = False
    try:
        cur.execute(
            """INSERT INTO runs (device_id, boot_id, run_number, event_id, time_ms, start_weight_g,
            min_weight_g, start_drop_threshold_g, stop_rise_threshold_g,
            status, firmware_version, queue_depth, received_at, measured_at, person_id, person_name, note)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, '')""",
            (
                run.device_id, run.boot_id, run.run_number, run.event_id, run.time_ms, run.start_weight_g,
                run.min_weight_g, run.start_drop_threshold_g, run.stop_rise_threshold_g,
                run.status, run.firmware_version, run.queue_depth, received_at, measured_at,
                person_id, person_name,
            ),
        )
        run_id = cur.lastrowid
    except sqlite3.IntegrityError as exc:
        existing = cur.execute(
            "SELECT id, received_at, person_id, person_name FROM runs WHERE device_id=? AND event_id=?",
            (run.device_id, run.event_id),
        ).fetchone()
        if not existing:
            # Kein Duplikat, sondern eine andere Bedingung der Datenbank verletzt
            raise HTTPException(status_code=422, detail=f"Lauf verletzt eine DB-Bedingung: {exc}") from exc
        duplicate = True
        run_id = existing["id"]
        received_at = existing["received_at"]
        person_id = existing["person_id"]
        person_name = existing["person_name"]

    cur.execute(
        """INSERT INTO devices (device_id, firmware_version, last_seen_at, last_boot_id, last_queue_depth)
        VALUES (?, ?, ?, ?, ?)
        ON CONFLICT(device_id) DO UPDATE SET
            firmware_version=excluded.firmware_version,
            last_seen_at=excluded.last_seen_at,
            last_boot_id=excluded.last_boot_id,
            last_queue_depth=excluded.last_queue_depth""",
        (run.device_id, run.firmware_version, utc_now_iso(), run.boot_id, run.queue_depth),
    )

    if not duplicate:
        _set_state(cur, "last_run_id", str(run_id))
        _set_state(cur, "last_run_received_at", received_at)
        _set_state(cur, "last_event", "run_received")

    return {
        "accepted": True,
        "duplicate": duplicate,
        "run_db_id": run_id,
        "received_at": received_at,
        "active_person_id": person_id,
        "active_person_name": person_name,
    }


@router.post("", status_code=201)
def receive_run(run: RunIn):
    with db_cursor() as (_, cur):
        return _insert_run(cur, run)


@router.post("/heartbeat")
def receive_heartbeat(payload: HeartbeatIn, request: Request):
    client_ip = request.client.host if request.client else None
    with db_cursor() as (_, cur):
        cur.execute(
            """INSERT INTO devices (device_id, firmware_version, last_seen_at, last_boot_id, last_queue_depth, last_ip)
            VALUES (?, ?, ?, ?, ?, ?)
            ON CONFLICT(device_id) DO UPDATE SET
                firmware_version=excluded.firmware_version,
                last_seen_at=excluded.last_seen_at,
                last_boot_id=COALESCE(excluded.last_boot_id, devices.last_boot_id),
                last_queue_depth=excluded.last_queue_depth,
                last_ip=excluded.last_ip""",
            (payload.device_id, payload.firmware_version, utc_now_iso(), payload.boot_id, payload.queue_depth, client_ip),
        )
    return {"ok": True}


@router.post("/batch")
def receive_batch(payload: RunBatchIn):
    with db_cursor() as (_, cur):
        results = [_insert_run(cur, run) for run in payload.runs]
    return {"accepted": True, "count": len(results), "results": results}


@router.post("/manual", status_code=201)
def create_manual_run(payload: ManualRunIn):
    """Manuell erfassten Lauf in die DB eintragen."""
    if payload.received_at is not None:
        stamp = payload.received_at
        if stamp.tzinfo is None:
            stamp = stamp.replace(tzinfo=timezone.utc)
        received_at = stamp.astimezone(timezone.utc).isoformat()
    else:
        received_at = utc_now_iso()

    with db_cursor() as (_, cur):
        person_name = None
        if payload.person_id is not None:
            person = cur.execute("SELECT name FROM persons WHERE id=?", (payload.person_id,)).fetchone()
            if not person:
                raise HTTPException(status_code=404, detail="Person nicht gefunden")
            person_name = person["name"]
        cur.execute(
            """INSERT INTO runs (device_id, boot_id, run_number, event_id, time_ms, start_weight_g,
            status, firmware_version, queue_depth, received_at, measured_at, person_id, person_name, note)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
            (
                "manual", 0, 0, f"manual-{uuid.uuid4().hex[:12]}", payload.time_ms, payload.start_weight_g,
                payload.status, "manual", 0, received_at, received_at, payload.person_id, person_name, payload.note,
            ),
        )
        run_id = cur.lastrowid
    return {"ok": True, "id": run_id}


def _like_pattern(text: str) -> str:
    """Suchtext fuer LIKE ... ESCAPE '\\': % und _ in der Eingabe sind normale Zeichen."""
    return "%" + text.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_") + "%"


@router.get("")
def list_runs(limit: int = 50, search: str | None = None, person_id: int | None = None, status: str | None = None, sort: str = "id_desc"):
    query = "SELECT * FROM runs WHERE 1=1"
    params: list = []
    if search:
        query += " AND (event_id LIKE ? ESCAPE '\\' OR device_id LIKE ? ESCAPE '\\' OR person_name LIKE ? ESCAPE '\\' OR note LIKE ? ESCAPE '\\')"
        params.extend([_like_pattern(search)] * 4)
    if person_id:
        query += " AND person_id=?"
        params.append(person_id)
    if status:
        query += " AND status=?"
        params.append(status)
    order_map = {
        "id_desc": "id DESC",
        "id_asc": "id ASC",
        "time_desc": "time_ms DESC",
        "time_asc": "time_ms ASC",
        "received_desc": "received_at DESC",
        "received_asc": "received_at ASC",
        "measured_desc": "COALESCE(measured_at, received_at) DESC",
        "measured_asc": "COALESCE(measured_at, received_at) ASC",
    }
    query += f" ORDER BY {order_map.get(sort, 'id DESC')} LIMIT ?"
    params.append(max(1, min(limit, 500)))
    with db_cursor() as (_, cur):
        rows = [dict(r) for r in cur.execute(query, params).fetchall()]
    return {"runs": rows, "count": len(rows)}


def _csv_safe(value):
    """Texte, die mit = + - @ (oder Tab/CR) beginnen, würde Excel/LibreOffice als Formel ausführen.
    Ein vorangestelltes Hochkomma macht sie zu normalem Text. Zahlen bleiben unverändert."""
    if isinstance(value, str) and value[:1] in ("=", "+", "-", "@", "\t", "\r"):
        return "'" + value
    return value


CSV_COLUMNS = [
    "id", "device_id", "boot_id", "run_number", "event_id", "time_ms", "start_weight_g",
    "min_weight_g", "start_drop_threshold_g", "stop_rise_threshold_g",
    "status", "firmware_version", "queue_depth", "received_at", "measured_at",
    "person_id", "person_name", "note",
]


@router.get("/export/csv")
def export_runs_csv():
    """Alle Läufe als CSV-Download (UTF-8 mit BOM, damit Excel Umlaute richtig anzeigt)."""
    with db_cursor() as (_, cur):
        rows = cur.execute(f"SELECT {', '.join(CSV_COLUMNS)} FROM runs ORDER BY id").fetchall()

    output = io.StringIO()
    writer = csv.writer(output, delimiter=";")
    writer.writerow(CSV_COLUMNS)
    for row in rows:
        writer.writerow([_csv_safe(v) for v in row])

    filename = f"wage_runs_{datetime.now(timezone.utc).strftime('%Y%m%d_%H%M%S')}.csv"
    return Response(
        content="﻿" + output.getvalue(),
        media_type="text/csv; charset=utf-8",
        headers={"Content-Disposition": f"attachment; filename={filename}"},
    )


@router.get("/{run_id}")
def get_run(run_id: int):
    with db_cursor() as (_, cur):
        row = cur.execute("SELECT * FROM runs WHERE id=?", (run_id,)).fetchone()
        if not row:
            raise HTTPException(status_code=404, detail="Lauf nicht gefunden")
        persons = [dict(p) for p in cur.execute("SELECT id, name FROM persons ORDER BY name").fetchall()]
    return {"run": dict(row), "persons": persons}


@router.put("/{run_id}")
def update_run(run_id: int, payload: RunUpdate):
    with db_cursor() as (_, cur):
        exists = cur.execute("SELECT id FROM runs WHERE id=?", (run_id,)).fetchone()
        if not exists:
            raise HTTPException(status_code=404, detail="Lauf nicht gefunden")
        if "person_id" in payload.model_fields_set and payload.person_id is None:
            # explizit null gesendet ("keine Person"): Zuordnung entfernen
            cur.execute("UPDATE runs SET person_id=NULL, person_name=NULL WHERE id=?", (run_id,))
        elif payload.person_id is not None:
            person = cur.execute("SELECT name FROM persons WHERE id=?", (payload.person_id,)).fetchone()
            if not person:
                raise HTTPException(status_code=404, detail="Person nicht gefunden")
            cur.execute("UPDATE runs SET person_id=?, person_name=? WHERE id=?", (payload.person_id, person[0], run_id))
        if payload.note is not None:
            cur.execute("UPDATE runs SET note=? WHERE id=?", (payload.note, run_id))
    return {"updated": True}


@router.delete("/{run_id}")
def delete_run(run_id: int):
    with db_cursor() as (_, cur):
        cur.execute("DELETE FROM runs WHERE id=?", (run_id,))
        if cur.rowcount == 0:
            raise HTTPException(status_code=404, detail="Lauf nicht gefunden")
    return {"deleted": True}
