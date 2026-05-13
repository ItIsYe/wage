import sqlite3

from fastapi import APIRouter, HTTPException

from .database import db_cursor, utc_now_iso
from .schemas import PersonCreate, PersonUpdate

router = APIRouter(prefix="/api/v1/persons", tags=["persons"])


def _set_active_person(cur, person_id: int):
    cur.execute(
        "INSERT INTO app_state(key, value) VALUES('active_person_id', ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
        (str(person_id),),
    )


@router.get("")
def list_persons():
    with db_cursor() as (_, cur):
        persons = [dict(r) for r in cur.execute("SELECT * FROM persons ORDER BY id").fetchall()]
        active = cur.execute("SELECT value FROM app_state WHERE key='active_person_id'").fetchone()
    return {"persons": persons, "active_person_id": int(active[0]) if active else 1}


@router.post("")
def create_person(payload: PersonCreate):
    with db_cursor() as (_, cur):
        try:
            cur.execute("INSERT INTO persons (name, created_at) VALUES (?, ?)", (payload.name, utc_now_iso()))
        except sqlite3.IntegrityError:
            raise HTTPException(status_code=409, detail="Name existiert bereits")
        pid = cur.lastrowid
        if payload.activate:
            _set_active_person(cur, pid)
    return {"created": True, "person_id": pid, "active": payload.activate}


@router.put("/{person_id}")
def rename_person(person_id: int, payload: PersonUpdate):
    if person_id == 1 and payload.name != "Unbekannt":
        raise HTTPException(status_code=400, detail="Standardperson darf nicht umbenannt werden")
    with db_cursor() as (_, cur):
        try:
            cur.execute("UPDATE persons SET name=? WHERE id=?", (payload.name, person_id))
        except sqlite3.IntegrityError:
            raise HTTPException(status_code=409, detail="Name existiert bereits")
        if cur.rowcount == 0:
            raise HTTPException(status_code=404, detail="Person nicht gefunden")
        cur.execute("UPDATE runs SET person_name=? WHERE person_id=?", (payload.name, person_id))
    return {"updated": True}


@router.delete("/{person_id}")
def delete_person(person_id: int):
    if person_id == 1:
        raise HTTPException(status_code=400, detail="Standardperson kann nicht gelöscht werden")
    with db_cursor() as (_, cur):
        active_row = cur.execute("SELECT value FROM app_state WHERE key='active_person_id'").fetchone()
        active = int(active_row[0]) if active_row else 1
        cur.execute("UPDATE runs SET person_id=1, person_name='Unbekannt' WHERE person_id=?", (person_id,))
        cur.execute("DELETE FROM persons WHERE id=?", (person_id,))
        if cur.rowcount == 0:
            raise HTTPException(status_code=404, detail="Person nicht gefunden")
        if active == person_id:
            _set_active_person(cur, 1)
            active = 1
    return {"deleted": True, "active_person_id": active}


@router.post("/{person_id}/activate")
def activate_person(person_id: int):
    with db_cursor() as (_, cur):
        exists = cur.execute("SELECT id FROM persons WHERE id=?", (person_id,)).fetchone()
        if not exists:
            raise HTTPException(status_code=404, detail="Person nicht gefunden")
        _set_active_person(cur, person_id)
    return {"activated": True, "active_person_id": person_id}
