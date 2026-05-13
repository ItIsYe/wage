from datetime import datetime, timezone
import socket
from fastapi import APIRouter

from .config import OFFLINE_THRESHOLD_SECONDS
from .database import db_cursor

router = APIRouter(prefix="/api/v1/status", tags=["status"])


@router.get("")
def status():
    now = datetime.now(timezone.utc)
    with db_cursor() as (_, cur):
        active_id = int(cur.execute("SELECT value FROM app_state WHERE key='active_person_id'").fetchone()[0])
        active_person = cur.execute("SELECT id, name FROM persons WHERE id=?", (active_id,)).fetchone()
        last_run = cur.execute("SELECT * FROM runs ORDER BY id DESC LIMIT 1").fetchone()
        last_device = cur.execute("SELECT * FROM devices ORDER BY last_seen_at DESC LIMIT 1").fetchone()
        led = cur.execute("SELECT value FROM app_state WHERE key='led_status'").fetchone()
        oled = cur.execute("SELECT value FROM app_state WHERE key='oled_status'").fetchone()

    last_contact = last_device["last_seen_at"] if last_device else None
    online = False
    if last_contact:
        seen = datetime.fromisoformat(last_contact)
        online = (now - seen).total_seconds() <= OFFLINE_THRESHOLD_SECONDS
    ip = socket.gethostbyname(socket.gethostname())
    return {
        "api_status": "ok",
        "database_status": "ok",
        "pi_ip": ip,
        "pi_url": f"http://{ip}:8000",
        "active_person": dict(active_person) if active_person else None,
        "last_run": dict(last_run) if last_run else None,
        "last_contact_to_scale": last_contact,
        "scale_online": online,
        "led_status": led[0] if led else "unknown",
        "oled_status": oled[0] if oled else "unknown",
    }
