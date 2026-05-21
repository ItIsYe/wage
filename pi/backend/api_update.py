from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path
import shutil
import subprocess

from fastapi import APIRouter, HTTPException

from .database import db_cursor

router = APIRouter(prefix="/api/v1/system/update", tags=["system-update"])

REPO_PATH = Path("/home/wage/wage")
UPDATE_SCOPE = "pi-only"
TARGET_BRANCH = "beta"
AP_BLOCK_REASON = "Update nicht möglich, solange der Pi als Access Point läuft. Bitte zuerst in den Haus-WLAN-Client-Modus wechseln."
GIT_TIMEOUT_SECONDS = 30
PIP_TIMEOUT_SECONDS = 180
SYSTEMCTL_TIMEOUT_SECONDS = 30


def _run(cmd: list[str], cwd: Path | None = None, timeout: int = GIT_TIMEOUT_SECONDS) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(cmd, cwd=str(cwd) if cwd else None, capture_output=True, text=True, timeout=timeout, check=False)
    except subprocess.TimeoutExpired as exc:
        raise HTTPException(status_code=500, detail=f"Timeout bei Befehl: {' '.join(cmd)}") from exc


def _read_state(keys: list[str]) -> dict[str, str]:
    with db_cursor() as (_, cur):
        q = ",".join(["?"] * len(keys))
        rows = cur.execute(f"SELECT key, value FROM app_state WHERE key IN ({q})", tuple(keys)).fetchall()
    out = {k: "" for k in keys}
    for row in rows:
        out[row["key"]] = row["value"]
    return out


def _network_mode() -> str:
    return _read_state(["network_mode"]).get("network_mode") or "ap"


def _require_client_mode() -> str:
    mode = _network_mode()
    if mode == "ap":
        raise HTTPException(status_code=400, detail=AP_BLOCK_REASON)
    return mode


def _ensure_repo_and_git() -> None:
    if shutil.which("git") is None:
        raise HTTPException(status_code=500, detail="git nicht verfügbar")
    if not REPO_PATH.exists() or not (REPO_PATH / ".git").exists():
        raise HTTPException(status_code=500, detail=f"Git-Repository nicht gefunden: {REPO_PATH}")


def _fetch_origin_beta() -> None:
    proc = _run(["git", "fetch", "origin", TARGET_BRANCH], cwd=REPO_PATH)
    if proc.returncode != 0:
        msg = proc.stderr.strip() or proc.stdout.strip() or "Unbekannter git fetch Fehler"
        raise HTTPException(status_code=500, detail=f"git fetch fehlgeschlagen: {msg}")


def _git_value(args: list[str], fallback: str = "") -> str:
    proc = _run(["git", *args], cwd=REPO_PATH)
    if proc.returncode != 0:
        return fallback
    return proc.stdout.strip()


def _changed_pi_files(local_commit: str, remote_commit: str) -> list[str]:
    if not local_commit or not remote_commit:
        return []
    proc = _run(["git", "diff", "--name-only", local_commit, remote_commit, "--", "pi"], cwd=REPO_PATH)
    if proc.returncode != 0:
        return []
    return [line.strip() for line in proc.stdout.splitlines() if line.strip().startswith("pi/")]


def _persist_update_state(status: str, local_commit: str, remote_commit: str, changed: list[str]) -> str:
    now = datetime.now(timezone.utc).isoformat()
    with db_cursor() as (_, cur):
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)", ("last_update_status", status))
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)", ("last_update_at", now))
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)", ("last_update_local_commit", local_commit))
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)", ("last_update_remote_commit", remote_commit))
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)", ("last_update_changed_pi_files", "\n".join(changed)))
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)", ("last_update_scope", UPDATE_SCOPE))
    return now


@router.get("/status")
def update_status():
    mode = _network_mode()
    state = _read_state([
        "last_update_status", "last_update_at", "last_update_local_commit", "last_update_remote_commit", "last_update_changed_pi_files", "last_update_scope"
    ])
    if mode == "ap":
        return {"ok": True, "allowed": False, "reason": "Update nicht möglich im AP-Modus", "network_mode": "ap", "update_scope": UPDATE_SCOPE, "pi_changes_available": False}

    _ensure_repo_and_git()
    local_commit = _git_value(["rev-parse", "HEAD"])
    remote_commit = _git_value(["rev-parse", f"origin/{TARGET_BRANCH}"])
    branch = _git_value(["rev-parse", "--abbrev-ref", "HEAD"], fallback="unknown")
    changed = _changed_pi_files(local_commit, remote_commit)
    return {
        "ok": True,
        "allowed": True,
        "reason": "",
        "network_mode": mode,
        "repo_path": str(REPO_PATH),
        "update_scope": UPDATE_SCOPE,
        "current_branch": branch,
        "local_commit": local_commit,
        "remote_commit": remote_commit,
        "pi_changes_available": len(changed) > 0,
        "changed_pi_files": changed,
        "last_update_status": state.get("last_update_status", ""),
        "last_update_at": state.get("last_update_at", ""),
    }


@router.post("/check")
def update_check():
    mode = _require_client_mode()
    _ensure_repo_and_git()
    _fetch_origin_beta()
    local_commit = _git_value(["rev-parse", "HEAD"])
    remote_commit = _git_value(["rev-parse", f"origin/{TARGET_BRANCH}"])
    branch = _git_value(["rev-parse", "--abbrev-ref", "HEAD"], fallback="unknown")
    changed = _changed_pi_files(local_commit, remote_commit)
    return {
        "ok": True,
        "allowed": True,
        "reason": "",
        "network_mode": mode,
        "repo_path": str(REPO_PATH),
        "update_scope": UPDATE_SCOPE,
        "current_branch": branch,
        "local_commit": local_commit,
        "remote_commit": remote_commit,
        "pi_changes_available": len(changed) > 0,
        "changed_pi_files": changed,
    }


@router.post("/apply")
def update_apply():
    mode = _require_client_mode()
    _ensure_repo_and_git()
    _fetch_origin_beta()
    local_commit = _git_value(["rev-parse", "HEAD"])
    remote_commit = _git_value(["rev-parse", f"origin/{TARGET_BRANCH}"])
    changed = _changed_pi_files(local_commit, remote_commit)
    if not changed:
        at = _persist_update_state("kein update nötig", local_commit, remote_commit, [])
        return {"ok": True, "status": "kein update nötig", "applied_at": at, "pi_changes_available": False, "changed_pi_files": []}

    local_changes = _run(["git", "status", "--porcelain", "--", "pi"], cwd=REPO_PATH)
    if local_changes.returncode != 0:
        raise HTTPException(status_code=500, detail=f"git status fehlgeschlagen: {local_changes.stderr.strip() or local_changes.stdout.strip()}")
    if local_changes.stdout.strip():
        raise HTTPException(status_code=400, detail="Lokale Änderungen unter /pi vorhanden. Bitte zuerst committen oder verwerfen.")

    checkout = _run(["git", "checkout", f"origin/{TARGET_BRANCH}", "--", "pi"], cwd=REPO_PATH)
    if checkout.returncode != 0:
        raise HTTPException(status_code=500, detail=f"pi checkout fehlgeschlagen: {checkout.stderr.strip() or checkout.stdout.strip()}")

    pip_proc = _run([str(REPO_PATH / "pi" / ".venv" / "bin" / "pip"), "install", "-r", "requirements.txt"], cwd=REPO_PATH / "pi", timeout=PIP_TIMEOUT_SECONDS)
    if pip_proc.returncode != 0:
        raise HTTPException(status_code=500, detail=f"requirements update fehlgeschlagen: {pip_proc.stderr.strip() or pip_proc.stdout.strip()}")

    for service in ["wage-pi-backend", "wage-pi-oled", "wage-pi-leds"]:
        proc = _run(["sudo", "systemctl", "restart", service], timeout=SYSTEMCTL_TIMEOUT_SECONDS)
        if proc.returncode != 0:
            raise HTTPException(status_code=500, detail=f"Service-Neustart fehlgeschlagen ({service}): {proc.stderr.strip() or proc.stdout.strip()}")

    at = _persist_update_state("update erfolgreich", local_commit, remote_commit, changed)
    return {
        "ok": True,
        "status": "update erfolgreich",
        "applied_at": at,
        "network_mode": mode,
        "update_scope": UPDATE_SCOPE,
        "local_commit": local_commit,
        "remote_commit": remote_commit,
        "pi_changes_available": True,
        "changed_pi_files": changed,
    }
