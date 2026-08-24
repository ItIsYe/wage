from datetime import datetime, timezone
import asyncio
import json
import os
import socket

from fastapi import APIRouter
from fastapi.responses import StreamingResponse

from .config import OFFLINE_THRESHOLD_SECONDS
from .database import db_cursor
from .api_network_config import _read_config

router = APIRouter(prefix="/api/v1/status", tags=["status"])


_ip_cache: str = ""
_ip_cache_at: float = 0.0
_IP_CACHE_TTL = 30.0  # IP nur alle 30s neu abfragen


def get_pi_ip() -> str:
    global _ip_cache, _ip_cache_at
    import time
    if _ip_cache and (time.monotonic() - _ip_cache_at) < _IP_CACHE_TTL:
        return _ip_cache
    try:
        import subprocess
        out = subprocess.check_output(
            ["nmcli", "-t", "-f", "IP4.ADDRESS", "device", "show"],
            text=True, timeout=3
        )
        for line in out.splitlines():
            if line.startswith("IP4.ADDRESS"):
                addr = line.split(":")[-1].split("/")[0].strip()
                if addr and not addr.startswith("127."):
                    _ip_cache = addr
                    _ip_cache_at = time.monotonic()
                    return _ip_cache
    except Exception:
        pass
    try:
        _ip_cache = os.popen("hostname -I").read().strip().split()[0]
        _ip_cache_at = time.monotonic()
        return _ip_cache
    except Exception:
        return "127.0.0.1"


_status_cache: dict = {}
_status_cache_at: float = 0.0
_STATUS_CACHE_TTL = 1.0  # Max 1s alter Status-Cache


def _build_status() -> dict:
    global _status_cache, _status_cache_at
    import time
    if _status_cache and (time.monotonic() - _status_cache_at) < _STATUS_CACHE_TTL:
        return _status_cache
    now = datetime.now(timezone.utc)
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
    online = False
    if last_contact:
        try:
            seen = datetime.fromisoformat(last_contact)
            online = (now - seen).total_seconds() <= OFFLINE_THRESHOLD_SECONDS
        except Exception:
            online = False

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
    """SSE-Stream: sendet bei jedem neuen Lauf ein Update ans Dashboard."""
    async def event_generator():
        last_run_id = None
        try:
            online_now = False
            while True:
                try:
                    data = _build_status()
                    online_now = data.get("scale_online", False)
                    current_run_id = data.get("last_run_id")
                    if current_run_id != last_run_id:
                        last_run_id = current_run_id
                        yield f"data: {json.dumps(data)}\n\n"
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
