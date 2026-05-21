from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path
import json
import shutil
import socket
import subprocess

from fastapi import APIRouter, HTTPException

from .database import db_cursor
from .schemas import NetworkConfigIn

router = APIRouter(prefix="/api/v1/config/network", tags=["network-config"])
SCRIPT_PATH = Path(__file__).resolve().parents[1] / "scripts" / "network_apply.sh"
CONFIG_JSON_PATH = Path(__file__).resolve().parents[1] / "data" / "network_config.json"

DEFAULTS = {
    "network_mode": "ap",
    "ap_ssid": "wage-net",
    "ap_password": "",
    "ap_ip": "192.168.50.1",
    "ap_dhcp_start": "192.168.50.50",
    "ap_dhcp_end": "192.168.50.150",
    "client_ssid": "",
    "client_password": "",
    "client_dhcp_enabled": "true",
    "last_network_apply_status": "never",
    "last_network_apply_at": "",
}


def _get_pi_ip() -> str:
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.connect(("8.8.8.8", 80))
        ip = sock.getsockname()[0]
        sock.close()
        return ip
    except Exception:
        return "127.0.0.1"


def _to_public(cfg: dict[str, str]) -> dict:
    mode = cfg.get("network_mode", "ap")
    api_target = f"http://{cfg.get('ap_ip','192.168.50.1')}:8000" if mode == "ap" else f"http://{_get_pi_ip()}:8000"
    return {
        "network_mode": mode,
        "ap_ssid": cfg.get("ap_ssid", "wage-net"),
        "ap_password_set": bool(cfg.get("ap_password")),
        "ap_ip": cfg.get("ap_ip", "192.168.50.1"),
        "ap_dhcp_start": cfg.get("ap_dhcp_start", "192.168.50.50"),
        "ap_dhcp_end": cfg.get("ap_dhcp_end", "192.168.50.150"),
        "client_ssid": cfg.get("client_ssid", ""),
        "client_password_set": bool(cfg.get("client_password")),
        "client_dhcp_enabled": str(cfg.get("client_dhcp_enabled", "true")).lower() == "true",
        "last_network_apply_status": cfg.get("last_network_apply_status", "never"),
        "last_network_apply_at": cfg.get("last_network_apply_at", ""),
        "current_pi_ip": _get_pi_ip(),
        "api_target": api_target,
    }


def _read_config() -> dict[str, str]:
    with db_cursor() as (_, cur):
        keys = tuple(DEFAULTS.keys())
        qmarks = ",".join(["?"] * len(keys))
        rows = cur.execute(f"SELECT key, value FROM app_state WHERE key IN ({qmarks})", keys).fetchall()
        cfg = DEFAULTS.copy()
        for r in rows:
            cfg[r["key"]] = r["value"]
        return cfg


def _store_json(cfg: dict[str, str]) -> None:
    CONFIG_JSON_PATH.parent.mkdir(parents=True, exist_ok=True)
    payload = {k: v for k, v in cfg.items() if k not in {"ap_password", "client_password"}}
    payload["ap_password_set"] = bool(cfg.get("ap_password"))
    payload["client_password_set"] = bool(cfg.get("client_password"))
    CONFIG_JSON_PATH.write_text(json.dumps(payload, indent=2), encoding="utf-8")


@router.get("")
def get_network_config():
    return _to_public(_read_config())


@router.post("")
def set_network_config(payload: NetworkConfigIn):
    cfg = _read_config()
    updates = payload.model_dump(exclude_unset=True)
    for k, v in updates.items():
        if k in {"ap_password", "client_password"}:
            if v:
                cfg[k] = v
        elif v is not None:
            cfg[k] = str(v).lower() if isinstance(v, bool) else str(v)

    with db_cursor() as (_, cur):
        for k, default in DEFAULTS.items():
            cur.execute("INSERT OR IGNORE INTO app_state (key, value) VALUES (?, ?)", (k, default))
            cur.execute("UPDATE app_state SET value=? WHERE key=?", (cfg.get(k, default), k))

    _store_json(cfg)
    return {"ok": True, "config": _to_public(cfg), "restart_recommended": True}


@router.post("/apply")
def apply_network_config():
    cfg = _read_config()
    now = datetime.now(timezone.utc).isoformat()
    status_msg = "applied"
    ok = True
    if not SCRIPT_PATH.exists():
        ok = False
        status_msg = f"error: script not found ({SCRIPT_PATH})"
    elif shutil.which("nmcli") is None:
        ok = False
        status_msg = "error: nmcli not available"
    else:
        try:
            proc = subprocess.run([str(SCRIPT_PATH)], check=False, capture_output=True, text=True, timeout=90)
            if proc.returncode != 0:
                ok = False
                status_msg = f"error: rc={proc.returncode} {proc.stderr.strip() or proc.stdout.strip()}"
            elif proc.stdout.strip():
                status_msg = f"applied: {proc.stdout.strip()}"
        except Exception as exc:
            ok = False
            status_msg = f"error: {exc}"

    with db_cursor() as (_, cur):
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?,?)", ("last_network_apply_status", status_msg))
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?,?)", ("last_network_apply_at", now))
    return {"ok": ok, "status": status_msg, "applied_at": now}


@router.get("/status")
def network_status():
    cfg = _to_public(_read_config())
    active = "unbekannt"
    if shutil.which("nmcli"):
        try:
            out = subprocess.check_output(["nmcli", "-t", "-f", "TYPE,STATE,CONNECTION", "device"], text=True, timeout=5)
            if "wifi:connected" in out:
                active = "Client aktiv" if cfg["network_mode"] == "client" else "AP aktiv"
        except Exception:
            active = "unbekannt"
    return {"status": active, **cfg}
