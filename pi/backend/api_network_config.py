from __future__ import annotations

import fcntl
from datetime import datetime, timezone
import ipaddress
from pathlib import Path
import re
import shutil
import subprocess

from fastapi import APIRouter, HTTPException, Request

from common.runtime import get_pi_ip

from .config_defaults import DEFAULTS
from .config_migration import ensure_config_defaults
from .database import db_cursor
from .schemas import NetworkConfigIn

router = APIRouter(prefix="/api/v1/config/network", tags=["network-config"])
SCRIPT_PATH = Path(__file__).resolve().parents[1] / "scripts" / "network_apply.sh"


def _to_public(cfg: dict[str, str]) -> dict:
    mode = cfg.get("network_mode", "ap")
    pi_ip = get_pi_ip()
    api_target = f"http://{cfg.get('ap_ip','192.168.50.1')}:8000" if mode == "ap" else f"http://{pi_ip}:8000"
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
        "current_pi_ip": pi_ip,
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
    try:
        ipaddress.IPv4Address(cfg["ap_ip"].strip())
    except ValueError:
        raise HTTPException(status_code=400, detail="AP-IP ist keine gültige IPv4-Adresse.")
    if len(cfg.get("ap_ssid", "").encode("utf-8")) > 32 or len(cfg.get("client_ssid", "").encode("utf-8")) > 32:
        raise HTTPException(status_code=400, detail="SSIDs dürfen höchstens 32 Bytes lang sein.")
    ap_password = cfg.get("ap_password", "")
    if ap_password and len(ap_password) < 8:
        raise HTTPException(status_code=400, detail="AP-Passwort muss mindestens 8 Zeichen lang sein.")
    if mode == "ap" and len(ap_password) < 8:
        raise HTTPException(status_code=400, detail="AP-Passwort muss gesetzt sein und mindestens 8 Zeichen haben.")
    if mode == "client" and not cfg.get("client_ssid", "").strip():
        raise HTTPException(status_code=400, detail="Client-SSID darf im Client-Modus nicht leer sein.")
    if mode == "ap":
        _validate_dhcp_range(cfg)


def _validate_dhcp_range(cfg: dict[str, str]) -> None:
    """DHCP-Bereich des AP: gueltige IPv4-Adressen im /24-Netz der AP-IP, Start <= Ende,
    AP-IP selbst nicht im Bereich (network_apply.sh nutzt ein /24-Netz)."""
    start_raw = cfg.get("ap_dhcp_start", "").strip()
    end_raw = cfg.get("ap_dhcp_end", "").strip()
    if not start_raw and not end_raw:
        return  # beide leer: NetworkManager-Standard-Bereich (siehe network_apply.sh)
    try:
        ap_ip = ipaddress.IPv4Address(cfg.get("ap_ip", "").strip())
        start = ipaddress.IPv4Address(start_raw)
        end = ipaddress.IPv4Address(end_raw)
    except ValueError:
        raise HTTPException(status_code=400, detail="AP-IP und DHCP-Start/-Ende müssen gültige IPv4-Adressen sein.")
    net = ipaddress.IPv4Network(f"{ap_ip}/24", strict=False)
    if start not in net or end not in net:
        raise HTTPException(status_code=400, detail=f"DHCP-Start und -Ende müssen im Netz {net} der AP-IP liegen.")
    if int(start) > int(end):
        raise HTTPException(status_code=400, detail="DHCP-Start darf nicht größer als DHCP-Ende sein.")
    if int(start) <= int(ap_ip) <= int(end):
        raise HTTPException(status_code=400, detail="Die AP-IP darf nicht im DHCP-Bereich liegen.")


_TERSE_SPLIT = re.compile(r"(?<!\\):")


def _terse_fields(line: str) -> list[str]:
    """nmcli -t trennt mit ':' und maskiert enthaltene Doppelpunkte als '\\:'."""
    return [part.replace("\\:", ":").replace("\\\\", "\\") for part in _TERSE_SPLIT.split(line)]


@router.get("/scan")
def scan_wifi_networks():
    """Verfügbare WLAN-Netzwerke scannen — funktioniert auch im AP-Modus."""
    try:
        devices = subprocess.check_output(["nmcli", "-t", "-f", "DEVICE,TYPE", "device", "status"], text=True, timeout=5)
        iface = None
        for line in devices.splitlines():
            fields = _terse_fields(line)
            if len(fields) >= 2 and fields[1] == "wifi":  # nicht "wifi-p2p"
                iface = fields[0]
                break
        if not iface:
            return {"networks": [], "error": "Kein WLAN-Interface gefunden"}

        # Rescan schlägt im AP-Modus fehl -> ignorieren, gecachte Ergebnisse lesen
        try:
            subprocess.run(["sudo", "nmcli", "device", "wifi", "rescan", "ifname", iface], timeout=8, capture_output=True)
        except (OSError, subprocess.SubprocessError):
            pass

        out = subprocess.check_output(
            ["nmcli", "-t", "-f", "SSID,SIGNAL,SECURITY", "device", "wifi", "list", "ifname", iface], text=True, timeout=10
        )
        seen: set[str] = set()
        networks = []
        for line in out.splitlines():
            fields = _terse_fields(line)
            if len(fields) < 2:
                continue
            ssid = fields[0].strip()
            if ssid and ssid not in seen:
                seen.add(ssid)
                networks.append({
                    "ssid": ssid,
                    "signal": int(fields[1]) if fields[1].isdigit() else 0,
                    "security": fields[2] if len(fields) > 2 else "",
                })
        networks.sort(key=lambda x: x["signal"], reverse=True)
        return {"networks": networks}
    except (OSError, subprocess.SubprocessError) as e:
        return {"networks": [], "error": f"WLAN-Scan fehlgeschlagen: {e}"}


@router.get("")
def get_network_config():
    ensure_config_defaults()
    return _to_public(_read_config())


@router.post("")
def set_network_config(payload: NetworkConfigIn):
    cfg = _read_config()
    updates = payload.model_dump(exclude_unset=True)
    changed: dict[str, str] = {}
    for k, v in updates.items():
        if k in {"ap_password", "client_password"}:
            if v:
                cfg[k] = changed[k] = v
        elif v is not None:
            cfg[k] = changed[k] = str(v).lower() if isinstance(v, bool) else str(v)

    _validate_network_config(cfg)

    # Nur die tatsächlich geänderten Keys schreiben. Früher wurden ALLE Defaults-Keys aus einem vorher
    # gelesenen Snapshot zurückgeschrieben und überschrieben so parallele Änderungen.
    with db_cursor() as (_, cur):
        for k, v in changed.items():
            cur.execute("INSERT INTO app_state(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (k, v))

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


_APPLY_LOCK_PATH = SCRIPT_PATH.parents[1] / "data" / "network_apply.lock"


@router.post("/apply")
async def apply_network_config():
    import asyncio
    # Prozessübergreifende Sperre (gunicorn startet mehrere Worker): zwei gleichzeitige
    # "Jetzt anwenden" würden sich beim Umschalten des WLANs gegenseitig stören.
    lock_file = open(_APPLY_LOCK_PATH, "w")
    try:
        fcntl.flock(lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        lock_file.close()
        raise HTTPException(status_code=409, detail="Netzwerk-Anwendung läuft bereits.")
    try:
        return await _apply_network_config_locked(asyncio)
    finally:
        lock_file.close()


async def _apply_network_config_locked(asyncio):
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
            proc = await asyncio.create_subprocess_exec(
                "sudo", str(SCRIPT_PATH),
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            try:
                stdout, stderr = await asyncio.wait_for(proc.communicate(), timeout=60.0)
            except asyncio.TimeoutError:
                proc.kill()
                await proc.wait()
                ok = False
                status_msg = "error: timeout (60s) — Netzwerk-Script hat nicht geantwortet"
            else:
                out = stdout.decode(errors="replace").strip()
                err = stderr.decode(errors="replace").strip()
                if proc.returncode != 0:
                    ok = False
                    # Letzten Fehler aus dem Log extrahieren
                    last_line = (err or out).splitlines()[-1] if (err or out) else f"rc={proc.returncode}"
                    status_msg = f"error: {last_line}"
                else:
                    last_line = out.splitlines()[-1] if out else "Netzwerk-Konfiguration angewendet"
                    status_msg = f"applied: {last_line}"
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
