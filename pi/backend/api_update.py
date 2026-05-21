from __future__ import annotations

import os
from datetime import datetime, timezone
from pathlib import Path
import shutil
import subprocess

from fastapi import APIRouter, HTTPException

from .config_migration import ensure_config_defaults
from .database import db_cursor

router = APIRouter(prefix="/api/v1/system/update", tags=["system-update"])

REPO_PATH = Path(os.getenv("WAGE_REPO_PATH", "/home/wage/wage")).resolve()
TARGET_BRANCH = os.getenv("WAGE_REPO_BRANCH", "beta")
UPDATE_SCOPE = "pi-only"
AP_BLOCK_REASON = "Update nicht möglich, solange der Pi als Access Point läuft. Bitte zuerst in den Haus-WLAN-Client-Modus wechseln."
GIT_TIMEOUT_SECONDS = 30
PIP_TIMEOUT_SECONDS = 180
SYSTEMCTL_TIMEOUT_SECONDS = 30
UPDATE_RELEVANT_PREFIXES = ("pi/backend/", "pi/frontend/", "pi/oled/", "pi/leds/", "pi/scripts/", "pi/systemd/")
UPDATE_RELEVANT_FILES = {"pi/requirements.txt", "pi/README.md"}


def _run(cmd: list[str], cwd: Path | None = None, timeout: int = GIT_TIMEOUT_SECONDS) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(cmd, cwd=str(cwd) if cwd else None, capture_output=True, text=True, timeout=timeout, check=False)
    except subprocess.TimeoutExpired as exc:
        raise HTTPException(status_code=500, detail=f"Timeout bei Befehl: {' '.join(cmd)}") from exc


def _git_cmd(*args: str) -> list[str]:
    return ["git", "-c", f"safe.directory={REPO_PATH}", *args]


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


def _fetch_origin_target() -> None:
    proc = _run(_git_cmd("fetch", "origin", TARGET_BRANCH), cwd=REPO_PATH)
    if proc.returncode != 0:
        msg = proc.stderr.strip() or proc.stdout.strip() or "Unbekannter git fetch Fehler"
        raise HTTPException(status_code=500, detail=f"git fetch fehlgeschlagen: {msg}")


def _git_value(args: list[str], fallback: str = "") -> str:
    proc = _run(_git_cmd(*args), cwd=REPO_PATH)
    if proc.returncode != 0:
        return fallback
    return proc.stdout.strip()


def _parse_status_paths(porcelain_output: str) -> list[str]:
    paths = []
    for line in porcelain_output.splitlines():
        if not line or len(line) < 4:
            continue
        candidate = line[3:]
        if " -> " in candidate:
            candidate = candidate.split(" -> ", 1)[1]
        normalized = candidate.strip().replace("\\", "/")
        if normalized.startswith("pi/"):
            paths.append(normalized)
    return paths


def is_protected_pi_runtime_path(path: str) -> bool:
    normalized = path.strip().replace("\\", "/")
    if normalized.startswith("pi/data/"):
        return True
    if normalized.startswith("pi/logs/"):
        return True
    if "/__pycache__/" in normalized:
        return True
    return normalized.endswith(".pyc")


def _is_update_relevant_pi_path(path: str) -> bool:
    return path in UPDATE_RELEVANT_FILES or path.startswith(UPDATE_RELEVANT_PREFIXES)


def _changed_pi_files(local_commit: str, remote_commit: str) -> tuple[list[str], list[str]]:
    if not local_commit or not remote_commit:
        return [], []
    proc = _run(_git_cmd("diff", "--name-only", local_commit, remote_commit, "--", "pi"), cwd=REPO_PATH)
    if proc.returncode != 0:
        return [], []
    paths = [line.strip() for line in proc.stdout.splitlines() if line.strip().startswith("pi/")]
    ignored_runtime = sorted({p for p in paths if is_protected_pi_runtime_path(p)})
    changed = sorted({p for p in paths if not is_protected_pi_runtime_path(p) and _is_update_relevant_pi_path(p)})
    return changed, ignored_runtime


def _local_pi_changes() -> tuple[list[str], list[str]]:
    proc = _run(_git_cmd("status", "--porcelain", "--", "pi"), cwd=REPO_PATH)
    if proc.returncode != 0:
        raise HTTPException(status_code=500, detail=f"git status fehlgeschlagen: {proc.stderr.strip() or proc.stdout.strip()}")
    paths = _parse_status_paths(proc.stdout)
    ignored = sorted({p for p in paths if is_protected_pi_runtime_path(p)})
    blocking = sorted({p for p in paths if not is_protected_pi_runtime_path(p) and _is_update_relevant_pi_path(p)})
    return ignored, blocking


def _persist_update_state(status: str, local_commit: str, remote_commit: str, changed: list[str], ignored_runtime: list[str], blocking_code: list[str]) -> str:
    now = datetime.now(timezone.utc).isoformat()
    with db_cursor() as (_, cur):
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)", ("last_update_status", status))
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)", ("last_update_at", now))
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)", ("last_update_local_commit", local_commit))
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)", ("last_update_remote_commit", remote_commit))
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)", ("last_update_changed_pi_files", "\n".join(changed)))
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)", ("last_update_ignored_runtime_files", "\n".join(ignored_runtime)))
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)", ("last_update_blocking_code_files", "\n".join(blocking_code)))
        cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)", ("last_update_scope", UPDATE_SCOPE))
    return now


@router.get("/status")
def update_status():
    mode = _network_mode()
    ignored_runtime, blocking_code = ([], [])
    state = _read_state([
        "last_update_status", "last_update_at", "last_update_local_commit", "last_update_remote_commit", "last_update_changed_pi_files", "last_update_scope"
    ])
    if mode == "ap":
        return {"ok": True, "allowed": False, "reason": "Update nicht möglich im AP-Modus", "network_mode": "ap", "update_scope": UPDATE_SCOPE, "pi_changes_available": False, "ignored_local_runtime_files": ignored_runtime, "blocking_local_code_files": blocking_code}

    _ensure_repo_and_git()
    local_commit = _git_value(["rev-parse", "HEAD"])
    remote_commit = _git_value(["rev-parse", f"origin/{TARGET_BRANCH}"])
    branch = _git_value(["rev-parse", "--abbrev-ref", "HEAD"], fallback="unknown")
    changed, ignored_remote_runtime = _changed_pi_files(local_commit, remote_commit)
    ignored_runtime, blocking_code = _local_pi_changes()
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
        "ignored_remote_runtime_files": ignored_remote_runtime,
        "ignored_local_runtime_files": ignored_runtime,
        "blocking_local_code_files": blocking_code,
        "last_update_status": state.get("last_update_status", ""),
        "last_update_at": state.get("last_update_at", ""),
    }


@router.post("/check")
def update_check():
    mode = _require_client_mode()
    _ensure_repo_and_git()
    _fetch_origin_target()
    local_commit = _git_value(["rev-parse", "HEAD"])
    remote_commit = _git_value(["rev-parse", f"origin/{TARGET_BRANCH}"])
    branch = _git_value(["rev-parse", "--abbrev-ref", "HEAD"], fallback="unknown")
    changed, ignored_remote_runtime = _changed_pi_files(local_commit, remote_commit)
    ignored_runtime, blocking_code = _local_pi_changes()
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
        "ignored_local_runtime_files": ignored_runtime,
        "blocking_local_code_files": blocking_code,
        "ignored_remote_runtime_files": ignored_remote_runtime,
    }


@router.post("/apply")
def update_apply():
    mode = _require_client_mode()
    _ensure_repo_and_git()
    _fetch_origin_target()
    local_commit = _git_value(["rev-parse", "HEAD"])
    remote_commit = _git_value(["rev-parse", f"origin/{TARGET_BRANCH}"])
    changed, ignored_remote_runtime = _changed_pi_files(local_commit, remote_commit)
    ignored_runtime, blocking_code = _local_pi_changes()

    if blocking_code:
        _persist_update_state("lokale code-änderungen", local_commit, remote_commit, changed, ignored_runtime, blocking_code)
        raise HTTPException(status_code=400, detail={"message": "Lokale Code-Änderungen unter /pi vorhanden. Update abgebrochen.", "blocking_local_code_files": blocking_code})

    if not changed:
        at = _persist_update_state("kein update nötig", local_commit, remote_commit, [], ignored_runtime + ignored_remote_runtime, [])
        return {"ok": True, "status": "kein update nötig", "applied_at": at, "pi_changes_available": False, "changed_pi_files": [], "ignored_local_runtime_files": ignored_runtime, "blocking_local_code_files": [], "ignored_remote_runtime_files": ignored_remote_runtime}

    checkout = _run(_git_cmd("checkout", f"origin/{TARGET_BRANCH}", "--", *changed), cwd=REPO_PATH)
    if checkout.returncode != 0:
        raise HTTPException(status_code=500, detail=f"pi checkout fehlgeschlagen: {checkout.stderr.strip() or checkout.stdout.strip()}")

    ensure_config_defaults()
    pip_proc = _run([str(REPO_PATH / "pi" / ".venv" / "bin" / "pip"), "install", "-r", "requirements.txt"], cwd=REPO_PATH / "pi", timeout=PIP_TIMEOUT_SECONDS)
    if pip_proc.returncode != 0:
        raise HTTPException(status_code=500, detail=f"requirements update fehlgeschlagen: {pip_proc.stderr.strip() or pip_proc.stdout.strip()}")

    for service in ["wage-pi-backend", "wage-pi-oled", "wage-pi-leds"]:
        proc = _run(["sudo", "systemctl", "restart", service], timeout=SYSTEMCTL_TIMEOUT_SECONDS)
        if proc.returncode != 0:
            raise HTTPException(status_code=500, detail=f"Service-Neustart fehlgeschlagen ({service}): {proc.stderr.strip() or proc.stdout.strip()}")

    at = _persist_update_state("update erfolgreich", local_commit, remote_commit, changed, ignored_runtime + ignored_remote_runtime, [])
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
        "ignored_local_runtime_files": ignored_runtime,
        "blocking_local_code_files": [],
        "ignored_remote_runtime_files": ignored_remote_runtime,
    }
