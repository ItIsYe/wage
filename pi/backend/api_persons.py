from fastapi import APIRouter, HTTPException

from .database import db_cursor
from .schemas import PersonCreate, PersonUpdate

router = APIRouter(prefix="/api/v1/persons", tags=["persons"])


def _set_active_person(person_id: int):
    with db_cursor() as (_, cur):
        cur.execute("UPDATE app_state SET value=? WHERE key='active_person_id'", (str(person_id),))


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
            cur.execute("INSERT INTO persons (name, created_at) VALUES (?, datetime('now'))", (payload.name.strip(),))
        except Exception as exc:
            raise HTTPException(status_code=400, detail=f"Person konnte nicht angelegt werden: {exc}") from exc
        pid = cur.lastrowid
    if payload.activate:
        _set_active_person(pid)
    return {"created": True, "person_id": pid, "active": payload.activate}


@router.put("/{person_id}")
def rename_person(person_id: int, payload: PersonUpdate):
    with db_cursor() as (_, cur):
        cur.execute("UPDATE persons SET name=? WHERE id=?", (payload.name.strip(), person_id))
        if cur.rowcount == 0:
            raise HTTPException(status_code=404, detail="Person nicht gefunden")
    return {"updated": True}


@router.delete("/{person_id}")
def delete_person(person_id: int):
    if person_id == 1:
        raise HTTPException(status_code=400, detail="Standardperson kann nicht gelöscht werden")
    with db_cursor() as (_, cur):
        cur.execute("DELETE FROM persons WHERE id=?", (person_id,))
        if cur.rowcount == 0:
            raise HTTPException(status_code=404, detail="Person nicht gefunden")
    return {"deleted": True}


@router.post("/{person_id}/activate")
def activate_person(person_id: int):
    with db_cursor() as (_, cur):
        exists = cur.execute("SELECT id FROM persons WHERE id=?", (person_id,)).fetchone()
        if not exists:
            raise HTTPException(status_code=404, detail="Person nicht gefunden")
    _set_active_person(person_id)
    return {"activated": True, "active_person_id": person_id}
