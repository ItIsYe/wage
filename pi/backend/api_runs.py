import sqlite3

from fastapi import APIRouter, HTTPException, Request

from .database import db_cursor, utc_now_iso
from .schemas import RunBatchIn, RunIn, RunUpdate

router = APIRouter(prefix="/api/v1/runs", tags=["runs"])


def _active_person(cur):
    row = cur.execute("SELECT value FROM app_state WHERE key='active_person_id'").fetchone()
    pid = int(row[0]) if row else 1
    person = cur.execute("SELECT id, name FROM persons WHERE id=?", (pid,)).fetchone()
    return (1, "Unbekannt") if not person else (person["id"], person["name"])


def _insert_run(cur, run: RunIn):
    received_at = utc_now_iso()
    person_id, person_name = _active_person(cur)
    duplicate = False
    try:
        cur.execute(
            """INSERT INTO runs (device_id, boot_id, run_number, event_id, time_ms, start_weight_g,
            min_weight_g, start_drop_threshold_g, stop_rise_threshold_g,
            status, firmware_version, queue_depth, received_at, person_id, person_name, note)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, '')""",
            (
                run.device_id,
                run.boot_id,
                run.run_number,
                run.event_id,
                run.time_ms,
                run.start_weight_g,
                run.min_weight_g,
                run.start_drop_threshold_g,
                run.stop_rise_threshold_g,
                run.status,
                run.firmware_version,
                run.queue_depth,
                received_at,
                person_id,
                person_name,
            ),
        )
        run_id = cur.lastrowid
    except sqlite3.IntegrityError:
        duplicate = True
        existing = cur.execute(
            "SELECT id, received_at, person_id, person_name FROM runs WHERE device_id=? AND event_id=?",
            (run.device_id, run.event_id),
        ).fetchone()
        if not existing:
            existing = cur.execute(
                "SELECT id, received_at, person_id, person_name FROM runs WHERE device_id=? AND boot_id=? AND run_number=?",
                (run.device_id, run.boot_id, run.run_number),
            ).fetchone()
        if not existing:
            raise HTTPException(status_code=500, detail="Duplikat konnte nicht aufgelöst werden")
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
        cur.execute("INSERT INTO app_state(key,value) VALUES('last_run_id',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (str(run_id),))
        cur.execute("INSERT INTO app_state(key,value) VALUES('last_run_received_at',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (received_at,))
        cur.execute("INSERT INTO app_state(key,value) VALUES('last_event','run_received') ON CONFLICT(key) DO UPDATE SET value=excluded.value")

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
        result = _insert_run(cur, run)
    return result


@router.post("/heartbeat")
def receive_heartbeat(payload: dict, request: Request):
    device_id = payload.get("device_id", "")
    firmware_version = payload.get("firmware_version", "")
    boot_id = payload.get("boot_id")
    queue_depth = payload.get("queue_depth")
    client_ip = request.client.host if request.client else None
    if not device_id:
        raise HTTPException(status_code=400, detail="device_id fehlt")
    with db_cursor() as (_, cur):
        cur.execute(
            """INSERT INTO devices (device_id, firmware_version, last_seen_at, last_boot_id, last_queue_depth, last_ip)
            VALUES (?, ?, ?, ?, ?, ?)
            ON CONFLICT(device_id) DO UPDATE SET
                firmware_version=excluded.firmware_version,
                last_seen_at=excluded.last_seen_at,
                last_boot_id=excluded.last_boot_id,
                last_queue_depth=excluded.last_queue_depth,
                last_ip=excluded.last_ip""",
            (device_id, firmware_version, utc_now_iso(), boot_id, queue_depth, client_ip),
        )
    return {"ok": True}
    with db_cursor() as (_, cur):
        return _insert_run(cur, payload)


@router.post("/batch")
def receive_batch(payload: RunBatchIn):
    results = []
    with db_cursor() as (_, cur):
        for run in payload.runs:
            results.append(_insert_run(cur, run))
    return {"accepted": True, "count": len(results), "results": results}


@router.post("/manual", status_code=201)
def create_manual_run(payload: dict):
    """Manuell erfassten Lauf in die DB eintragen."""
    weight = payload.get("start_weight_g")
    if weight is None:
        raise HTTPException(status_code=400, detail="start_weight_g fehlt")
    try:
        weight = float(weight)
    except (TypeError, ValueError):
        raise HTTPException(status_code=400, detail="start_weight_g muss eine Zahl sein")

    received_at = payload.get("received_at") or utc_now_iso()
    person_id = payload.get("person_id")
    person_name = None
    if person_id:
        with db_cursor() as (_, cur):
            p = cur.execute("SELECT name FROM persons WHERE id=?", (int(person_id),)).fetchone()
            if p:
                person_name = p["name"]

    import uuid
    event_id = f"manual-{uuid.uuid4().hex[:12]}"

    with db_cursor() as (_, cur):
        cur.execute(
            """INSERT INTO runs (device_id, boot_id, run_number, event_id, time_ms, start_weight_g,
            status, firmware_version, queue_depth, received_at, person_id, person_name, note)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
            (
                "manual",
                0,
                0,
                event_id,
                int(payload.get("time_ms", 0)),
                weight,
                payload.get("status", "manual"),
                "manual",
                0,
                received_at,
                int(person_id) if person_id else None,
                person_name,
                payload.get("note", ""),
            ),
        )
        run_id = cur.lastrowid
    return {"ok": True, "id": run_id}


@router.get("")
def list_runs(limit: int = 50, search: str | None = None, person_id: int | None = None, status: str | None = None, sort: str = "id_desc"):
    query = "SELECT * FROM runs WHERE 1=1"
    params = []
    if search:
        query += " AND (event_id LIKE ? OR device_id LIKE ? OR person_name LIKE ? OR note LIKE ?)"
        params.extend([f"%{search}%"] * 4)
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
    }
    query += f" ORDER BY {order_map.get(sort, 'id DESC')} LIMIT ?"
    params.append(max(1, min(limit, 500)))
    with db_cursor() as (_, cur):
        rows = [dict(r) for r in cur.execute(query, params).fetchall()]
    return {"runs": rows, "count": len(rows)}


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
        if payload.person_id is not None:
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
