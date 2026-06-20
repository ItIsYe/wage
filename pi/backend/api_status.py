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


def get_pi_ip() -> str:
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
                    return addr
    except Exception:
        pass
    try:
        return os.popen("hostname -I").read().strip().split()[0]
    except Exception:
        return "127.0.0.1"


def _build_status() -> dict:
    now = datetime.now(timezone.utc)
    with db_cursor() as (_, cur):
        app = {r["key"]: r["value"] for r in cur.execute("SELECT key,value FROM app_state")}
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
    return {
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


@router.get("")
def status():
    return _build_status()


@router.get("/stream")
async def status_stream():
    """SSE-Stream: sendet bei jedem neuen Lauf ein Update ans Dashboard."""
    async def event_generator():
        last_run_id = None
        try:
            while True:
                try:
                    data = _build_status()
                    current_run_id = data.get("last_run_id")
                    if current_run_id != last_run_id:
                        last_run_id = current_run_id
                        yield f"data: {json.dumps(data)}\n\n"
                    else:
                        # Heartbeat alle 5s damit die Verbindung offen bleibt
                        yield ": heartbeat\n\n"
                except Exception:
                    yield ": error\n\n"
                await asyncio.sleep(2)
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
