from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path
import shutil
import socket
import subprocess

from fastapi import APIRouter, HTTPException, Request

from .config_defaults import DEFAULTS
from .config_migration import ensure_config_defaults
from .database import db_cursor
from .schemas import NetworkConfigIn

router = APIRouter(prefix="/api/v1/config/network", tags=["network-config"])
SCRIPT_PATH = Path(__file__).resolve().parents[1] / "scripts" / "network_apply.sh"


def _get_pi_ip() -> str:
    import fcntl, struct
    try:
        # Versuche IP über aktives WLAN-Interface zu ermitteln (funktioniert ohne Internet)
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
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.connect(("192.168.50.1", 80))
        ip = sock.getsockname()[0]
        sock.close()
        if ip and not ip.startswith("0."):
            return ip
    except Exception:
        pass
    return "127.0.0.1"


def _to_public(cfg: dict[str, str]) -> dict:
    mode = cfg.get("network_mode", "ap")
    api_target = f"http://{cfg.get('ap_ip','192.168.50.1')}:8000" if mode == "ap" else f"http://{_get_pi_ip()}:8000"
    ap_password_set = bool(cfg.get("ap_password"))
    return {
        "network_mode": mode,
        "ap_ssid": cfg.get("ap_ssid", "wage-net"),
        "ap_password_set": ap_password_set,
        "ap_security": "wpa-psk",
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


def _validate_network_config(cfg: dict[str, str]) -> None:
    mode = cfg.get("network_mode", "ap")
    if mode not in {"ap", "client"}:
        raise HTTPException(status_code=400, detail="Ungültiger Netzwerkmodus. Erlaubt: ap oder client.")
    if not cfg.get("ap_ssid", "").strip():
        raise HTTPException(status_code=400, detail="AP-SSID darf nicht leer sein.")
    if not cfg.get("ap_ip", "").strip():
        raise HTTPException(status_code=400, detail="AP-IP darf nicht leer sein.")
    ap_password = cfg.get("ap_password", "")
    if ap_password and len(ap_password) < 8:
        raise HTTPException(status_code=400, detail="AP-Passwort muss mindestens 8 Zeichen lang sein.")
    if mode == "ap" and len(ap_password) < 8:
        raise HTTPException(status_code=400, detail="AP-Passwort muss gesetzt sein und mindestens 8 Zeichen haben.")
    if mode == "client" and not cfg.get("client_ssid", "").strip():
        raise HTTPException(status_code=400, detail="Client-SSID darf im Client-Modus nicht leer sein.")


@router.get("")
def get_network_config():
    ensure_config_defaults()
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

    _validate_network_config(cfg)

    with db_cursor() as (_, cur):
        for k, default in DEFAULTS.items():
            cur.execute("INSERT OR IGNORE INTO app_state (key, value) VALUES (?, ?)", (k, default))
            cur.execute("UPDATE app_state SET value=? WHERE key=?", (cfg.get(k, default), k))

    ensure_config_defaults()
    return {"ok": True, "config": _to_public(_read_config()), "restart_recommended": True}




def _require_local(request: Request) -> None:
    client_ip = request.client.host if request.client else ""
    if client_ip not in ("127.0.0.1", "::1"):
        raise HTTPException(status_code=403, detail="Nur lokal erlaubt.")


@router.get("/secret/ap-password")
def get_ap_password_secret(request: Request):
    _require_local(request)
    cfg = _read_config()
    return {"ok": True, "value": cfg.get("ap_password", "")}


@router.get("/secret/client-password")
def get_client_password_secret(request: Request):
    _require_local(request)
    cfg = _read_config()
    return {"ok": True, "value": cfg.get("client_password", "")}


@router.post("/apply")
def apply_network_config():
    now = datetime.now(timezone.utc).isoformat()
    with db_cursor() as (_, cur):
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?,?)", ("last_network_apply_status", "applying"))
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?,?)", ("last_network_apply_at", now))

    status_msg = "applied"
    ok = True
    if not SCRIPT_PATH.exists():
        ok = False
        status_msg = f"error: script not found ({SCRIPT_PATH})"
    elif shutil.which("nmcli") is None:
        ok = False
        status_msg = "error: nmcli not available"
    elif shutil.which("sqlite3") is None:
        ok = False
        status_msg = "error: sqlite3 not available"
    else:
        try:
            proc = subprocess.run([str(SCRIPT_PATH)], check=False, capture_output=True, text=True, timeout=90)
            if proc.returncode != 0:
                ok = False
                status_msg = f"error: rc={proc.returncode} {(proc.stderr.strip() or proc.stdout.strip())}"
            else:
                out = proc.stdout.strip()
                status_msg = f"applied: {out}" if out else "applied: Netzwerk-Konfiguration angewendet"
        except Exception as exc:
            ok = False
            status_msg = f"error: {exc}"

    with db_cursor() as (_, cur):
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?,?)", ("last_network_apply_status", status_msg))
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?,?)", ("last_network_apply_at", now))
    return {
        "ok": ok,
        "status": status_msg,
        "applied_at": now,
        "message": "Netzwerk-Konfiguration angewendet" if ok else status_msg,
    }


@router.get("/status")
def network_status():
    cfg = _to_public(_read_config())
    active = "unbekannt"
    current_connection = ""
    if shutil.which("nmcli"):
        try:
            current_connection = subprocess.check_output(
                ["nmcli", "-t", "-f", "NAME,TYPE,DEVICE", "connection", "show", "--active"], text=True, timeout=5
            ).strip()
            out = subprocess.check_output(["nmcli", "-t", "-f", "TYPE,STATE,CONNECTION", "device"], text=True, timeout=5)
            if "wifi:connected" in out:
                active = "Client aktiv" if cfg["network_mode"] == "client" else "AP aktiv"
        except Exception:
            active = "unbekannt"
    return {"status": active, "current_nmcli_connection": current_connection, **cfg}
