import sqlite3
from fastapi import APIRouter, HTTPException, Query

from .database import db_cursor, utc_now_iso
from .schemas import RunBatchIn, RunIn

router = APIRouter(prefix="/api/v1/runs", tags=["runs"])


def _active_person(cur):
    row = cur.execute("SELECT value FROM app_state WHERE key='active_person_id'").fetchone()
    pid = int(row[0]) if row else 1
    person = cur.execute("SELECT id, name FROM persons WHERE id=?", (pid,)).fetchone()
    return person["id"], person["name"]


def _insert_run(cur, run: RunIn):
    received_at = utc_now_iso()
    person_id, person_name = _active_person(cur)
    duplicate = False
    try:
        cur.execute(
            """INSERT INTO runs (device_id, boot_id, run_number, event_id, time_ms, start_weight_g, status,
            firmware_version, queue_depth, received_at, person_id, person_name, note)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, '')""",
            (
                run.device_id, run.boot_id, run.run_number, run.event_id, run.time_ms,
                run.start_weight_g, run.status, run.firmware_version, run.queue_depth,
                received_at, person_id, person_name,
            ),
        )
        run_id = cur.lastrowid
    except sqlite3.IntegrityError:
        duplicate = True
        existing = cur.execute(
            "SELECT id, received_at, person_id, person_name FROM runs WHERE device_id=? AND event_id=?",
            (run.device_id, run.event_id),
        ).fetchone()
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

    return {
        "accepted": True,
        "duplicate": duplicate,
        "run_db_id": run_id,
        "received_at": received_at,
        "active_person_id": person_id,
        "active_person_name": person_name,
    }


@router.post("")
def receive_run(payload: RunIn):
    with db_cursor() as (_, cur):
        return _insert_run(cur, payload)


@router.post("/batch")
def receive_batch(payload: RunBatchIn):
    results = []
    with db_cursor() as (_, cur):
        for run in payload.runs:
            results.append(_insert_run(cur, run))
    return {"accepted": True, "count": len(results), "results": results}


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

    order_map = {"id_desc": "id DESC", "id_asc": "id ASC", "time_desc": "time_ms DESC", "time_asc": "time_ms ASC"}
    query += f" ORDER BY {order_map.get(sort, 'id DESC')} LIMIT ?"
    params.append(max(1, min(limit, 500)))

    with db_cursor() as (_, cur):
        rows = [dict(r) for r in cur.execute(query, params).fetchall()]
    return {"runs": rows, "count": len(rows)}


@router.put("/{run_id}")
def update_run(run_id: int, person_id: int | None = None, note: str | None = None):
    with db_cursor() as (_, cur):
        if person_id is not None:
            person = cur.execute("SELECT name FROM persons WHERE id=?", (person_id,)).fetchone()
            if not person:
                raise HTTPException(status_code=404, detail="Person nicht gefunden")
            cur.execute("UPDATE runs SET person_id=?, person_name=? WHERE id=?", (person_id, person[0], run_id))
        if note is not None:
            cur.execute("UPDATE runs SET note=? WHERE id=?", (note, run_id))
    return {"updated": True}


@router.delete("/{run_id}")
def delete_run(run_id: int):
    with db_cursor() as (_, cur):
        cur.execute("DELETE FROM runs WHERE id=?", (run_id,))
    return {"deleted": True}
