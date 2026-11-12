import asyncio
import json
import time

from fastapi import APIRouter
from fastapi.responses import StreamingResponse

from common.runtime import get_pi_ip, seconds_since

from .config import OFFLINE_THRESHOLD_SECONDS
from .database import db_cursor
from .api_network_config import _read_config

router = APIRouter(prefix="/api/v1/status", tags=["status"])


_status_cache: dict = {}
_status_cache_at: float = 0.0
_STATUS_CACHE_TTL = 1.0  # Max 1s alter Status-Cache


def _build_status() -> dict:
    global _status_cache, _status_cache_at
    if _status_cache and (time.monotonic() - _status_cache_at) < _STATUS_CACHE_TTL:
        return _status_cache
    with db_cursor() as (_, cur):
        # Nur benötigte Keys laden statt alle
        needed = ("active_person_id","last_run_id","last_run_received_at","last_event",
                  "api_status","database_status","led_status","oled_status")
        app = {r["key"]: r["value"] for r in cur.execute(
            f"SELECT key,value FROM app_state WHERE key IN ({','.join('?'*len(needed))})",
            needed
        )}
        active_id = int(app.get("active_person_id", "1"))
        active_person = cur.execute("SELECT id, name FROM persons WHERE id=?", (active_id,)).fetchone()
        last_run = cur.execute("SELECT * FROM runs ORDER BY id DESC LIMIT 1").fetchone()
        last_runs = [dict(r) for r in cur.execute("SELECT id, run_number, time_ms, start_weight_g, status, received_at, person_name FROM runs ORDER BY id DESC LIMIT 5").fetchall()]
        last_device = cur.execute("SELECT * FROM devices ORDER BY last_seen_at DESC LIMIT 1").fetchone()

    last_contact = last_device["last_seen_at"] if last_device else None
    age = seconds_since(last_contact)
    online = age is not None and age <= OFFLINE_THRESHOLD_SECONDS

    ip = get_pi_ip()
    net = _read_config()
    network_mode = net.get("network_mode", "ap")
    api_target_for_esp = f"http://{net.get('ap_ip','192.168.50.1')}:8000/api/v1/runs" if network_mode == "ap" else f"http://{ip}:8000/api/v1/runs"
    result = {
        "api_status": app.get("api_status", "ok"),
        "database_status": app.get("database_status", "ok"),
        "pi_ip": ip,
        "pi_url": f"http://{ip}:8000",
        "active_person": dict(active_person) if active_person else None,
        "last_run": dict(last_run) if last_run else None,
        "recent_runs": last_runs,
        "last_contact_to_scale": last_contact,
        "scale_online": online,
        "led_status": app.get("led_status", "unknown"),
        "oled_status": app.get("oled_status", "unknown"),
        "last_run_id": app.get("last_run_id"),
        "last_run_received_at": app.get("last_run_received_at"),
        "last_event": app.get("last_event"),
        "last_device": dict(last_device) if last_device else None,
        "scale_ip": last_device["last_ip"] if last_device else None,
        "network_mode": network_mode,
        "ap_ssid": net.get("ap_ssid", "wage-net"),
        "api_target_for_esp": api_target_for_esp,
    }
    _status_cache = result
    _status_cache_at = time.monotonic()
    return result


@router.get("")
def status():
    return _build_status()


@router.get("/stream")
async def status_stream():
    """SSE-Stream: sendet ein Update ans Dashboard, sobald sich der Status ändert.

    Verglichen wird der gesamte Status, nicht nur die Lauf-ID. Sonst bliebe z.B.
    "Waage online" im Dashboard stehen, obwohl die Waage längst offline ist.
    """
    async def event_generator():
        last_payload = None
        try:
            online_now = False
            while True:
                try:
                    data = await asyncio.to_thread(_build_status)
                    online_now = data.get("scale_online", False)
                    payload = json.dumps(data, sort_keys=True)
                    if payload != last_payload:
                        last_payload = payload
                        yield f"data: {payload}\n\n"
                    else:
                        # Heartbeat alle 5s damit die Verbindung offen bleibt
                        yield ": heartbeat\n\n"
                except Exception:
                    yield ": error\n\n"
                await asyncio.sleep(2 if online_now else 5)
        except asyncio.CancelledError:
            pass

    return StreamingResponse(
        event_generator(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",
        },
    )
