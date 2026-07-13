from __future__ import annotations

import hashlib
import os
from datetime import datetime, timezone
from pathlib import Path
import shutil
import subprocess
import sys

from fastapi import APIRouter, HTTPException

from .config_migration import ensure_config_defaults
from .database import db_cursor

router = APIRouter(prefix="/api/v1/system/update", tags=["system-update"])

REPO_PATH = Path(os.getenv("WAGE_REPO_PATH", "/home/wage/wage")).resolve()
TARGET_BRANCH = os.getenv("WAGE_REPO_BRANCH", "beta")
UPDATE_SCOPE = "pi-only"
AP_BLOCK_REASON = "Updates sind nur im Haus-WLAN-Client-Modus möglich"
GIT_TIMEOUT_SECONDS = 30
PIP_TIMEOUT_SECONDS = 180
SYSTEMCTL_TIMEOUT_SECONDS = 30
UPDATE_RELEVANT_DIRS = ("pi/backend", "pi/frontend", "pi/oled", "pi/leds", "pi/scripts", "pi/systemd")
UPDATE_RELEVANT_FILES = ("pi/requirements.txt", "pi/README.md")
PROTECTED_RUNTIME_PATHS = ["pi/data/", "pi/logs/"]
PROTECTED_RUNTIME_FILES = [
    "pi/data/wage_pi.sqlite3",
    "pi/data/wage_pi.sqlite3-shm",
    "pi/data/wage_pi.sqlite3-wal",
    "pi/logs/network_apply.log",
]


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


def _has_internet() -> bool:
    """Prüft ob Internet verfügbar ist (eth0 mit Default-Route oder Client-Modus)."""
    import subprocess
    try:
        result = subprocess.run(
            ["ip", "route", "show", "default"],
            capture_output=True, text=True, timeout=3
        )
        return "default" in result.stdout
    except Exception:
        return False


def _require_internet() -> str:
    """Erlaubt Update-Aktionen wenn Internet verfügbar ist — egal ob AP- oder Client-Modus."""
    mode = _network_mode()
    if not _has_internet():
        raise HTTPException(
            status_code=400,
            detail="Kein Internet verfügbar. Bitte LAN-Kabel einstecken oder in den Client-Modus wechseln."
        )
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


def is_protected_pi_runtime_path(path: str) -> bool:
    normalized = path.strip().replace("\\", "/")
    if any(normalized.startswith(prefix) for prefix in PROTECTED_RUNTIME_PATHS):
        return True
    if "/__pycache__/" in normalized:
        return True
    return normalized.endswith(".pyc")


def _is_update_relevant_pi_path(path: str) -> bool:
    normalized = path.replace("\\", "/")
    if normalized in UPDATE_RELEVANT_FILES:
        return True
    return any(normalized.startswith(f"{d}/") for d in UPDATE_RELEVANT_DIRS)


def _list_remote_update_files_with_hashes() -> dict[str, str]:
    pathspecs = [*UPDATE_RELEVANT_DIRS, *UPDATE_RELEVANT_FILES]
    proc = _run(_git_cmd("ls-tree", "-r", f"origin/{TARGET_BRANCH}", "--", *pathspecs), cwd=REPO_PATH)
    if proc.returncode != 0:
        raise HTTPException(status_code=500, detail=f"Remote-Dateiliste fehlgeschlagen: {proc.stderr.strip() or proc.stdout.strip()}")
    files: dict[str, str] = {}
    for line in proc.stdout.splitlines():
        if "\t" not in line:
            continue
        meta, file_path = line.split("\t", 1)
        parts = meta.split()
        if len(parts) < 3:
            continue
        blob_hash = parts[2]
        normalized = file_path.strip().replace("\\", "/")
        if normalized.startswith("pi/") and _is_update_relevant_pi_path(normalized):
            files[normalized] = blob_hash
    return files


def _list_local_update_files() -> set[str]:
    collected: set[str] = set()
    for rel_dir in UPDATE_RELEVANT_DIRS:
        base = REPO_PATH / rel_dir
        if not base.exists():
            continue
        for p in base.rglob("*"):
            if not p.is_file():
                continue
            rel = p.relative_to(REPO_PATH).as_posix()
            if is_protected_pi_runtime_path(rel):
                continue
            collected.add(rel)
    for rel_file in UPDATE_RELEVANT_FILES:
        p = REPO_PATH / rel_file
        if p.exists() and p.is_file() and not is_protected_pi_runtime_path(rel_file):
            collected.add(rel_file)
    return collected


def _local_blob_hash(path: str) -> str:
    file_path = REPO_PATH / path
    if not file_path.exists() or not file_path.is_file():
        return ""
    data = file_path.read_bytes()
    header = f"blob {len(data)}\0".encode("utf-8")
    return hashlib.sha1(header + data).hexdigest()


def _calculate_pi_sync_sets() -> tuple[list[str], list[str], list[str]]:
    remote = _list_remote_update_files_with_hashes()
    local = _list_local_update_files()
    changed = sorted([path for path, remote_hash in remote.items() if _local_blob_hash(path) != remote_hash])
    obsolete = sorted([path for path in local if path not in remote and not is_protected_pi_runtime_path(path)])
    return changed, obsolete, sorted(PROTECTED_RUNTIME_FILES)


def _persist_update_state(status: str, local_commit: str, remote_commit: str, changed: list[str], obsolete: list[str], protected_runtime: list[str], ui_state: str = "idle", ui_message: str = "", progress_step: str = "", progress_percent: int = 0, local_code_overwritten: bool = True) -> str:
    now = datetime.now(timezone.utc).isoformat()
    with db_cursor() as (_, cur):
        entries = {
            "last_update_status": status,
            "last_update_at": now,
            "last_update_local_commit": local_commit,
            "last_update_remote_commit": remote_commit,
            "last_update_changed_pi_files": "\n".join(changed),
            "last_update_obsolete_pi_files": "\n".join(obsolete),
            "last_update_protected_runtime_files": "\n".join(protected_runtime),
            "last_update_scope": UPDATE_SCOPE,
            "last_update_ui_state": ui_state,
            "last_update_ui_message": ui_message,
            "last_update_progress_step": progress_step,
            "last_update_progress_percent": str(progress_percent),
            "last_update_local_code_overwritten": "1" if local_code_overwritten else "0",
        }
        for key, value in entries.items():
            cur.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)", (key, value))
    return now


def _response_payload(mode: str, state: dict[str, str], local_commit: str, remote_commit: str, changed: list[str], obsolete: list[str], protected_runtime: list[str], ui_state: str, ui_message: str, progress_step: str = "", progress_percent: int = 0, can_apply: bool = False) -> dict:
    return {
        "ok": True,
        "allowed": mode == "client",
        "network_mode": mode,
        "update_scope": UPDATE_SCOPE,
        "local_commit": local_commit,
        "remote_commit": remote_commit,
        "ui_state": ui_state,
        "ui_message": ui_message,
        "can_check": mode == "client",
        "can_apply": can_apply,
        "progress_step": progress_step or state.get("last_update_progress_step", ""),
        "progress_percent": progress_percent if progress_percent else int(state.get("last_update_progress_percent", "0") or 0),
        "changed_pi_files": changed,
        "obsolete_pi_files": obsolete,
        "protected_runtime_files": protected_runtime,
        "local_code_overwritten": True,
        "last_update_status": state.get("last_update_status", ""),
        "last_update_at": state.get("last_update_at", ""),
    }


def _calculate_update_status() -> tuple[str, str, list[str], list[str], list[str]]:
    local_commit = _git_value(["rev-parse", "HEAD"])
    remote_commit = _git_value(["rev-parse", f"origin/{TARGET_BRANCH}"])
    changed, obsolete, protected = _calculate_pi_sync_sets()
    return local_commit, remote_commit, changed, obsolete, protected


def _remove_obsolete_files(obsolete: list[str]) -> None:
    for rel in obsolete:
        p = REPO_PATH / rel
        if p.exists() and p.is_file() and not is_protected_pi_runtime_path(rel):
            p.unlink()


def _run_background_update_job() -> None:
    try:
        _ensure_repo_and_git()
        _fetch_origin_target()
        local_commit, remote_commit, changed, obsolete, protected = _calculate_update_status()
        if not changed and not obsolete:
            return
        _persist_update_state("installiere update...", local_commit, remote_commit, changed, obsolete, protected, ui_state="updating", ui_message="Pi-Code-Sync wird durchgeführt...", progress_step="Pi-Code wird synchronisiert...", progress_percent=20)
        if changed:
            proc = _run(_git_cmd("checkout", f"origin/{TARGET_BRANCH}", "--", *changed), cwd=REPO_PATH)
            if proc.returncode != 0:
                raise RuntimeError(proc.stderr.strip() or proc.stdout.strip() or "git checkout fehlgeschlagen")
        _persist_update_state("installiere update...", local_commit, remote_commit, changed, obsolete, protected, ui_state="updating", ui_message="Pi-Code-Sync wird durchgeführt...", progress_step="Alte Pi-Code-Dateien werden entfernt...", progress_percent=40)
        _remove_obsolete_files(obsolete)
        _persist_update_state("installiere update...", local_commit, remote_commit, changed, obsolete, protected, ui_state="updating", ui_message="Pi-Code-Sync wird durchgeführt...", progress_step="Konfiguration wird migriert...", progress_percent=60)
        ensure_config_defaults()
        _persist_update_state("installiere update...", local_commit, remote_commit, changed, obsolete, protected, ui_state="updating", ui_message="Pi-Code-Sync wird durchgeführt...", progress_step="Abhängigkeiten werden geprüft...", progress_percent=75)
        _run([str(REPO_PATH / "pi" / ".venv" / "bin" / "pip"), "install", "-r", "requirements.txt"], cwd=REPO_PATH / "pi", timeout=PIP_TIMEOUT_SECONDS)
        _persist_update_state("installiere update...", local_commit, remote_commit, changed, obsolete, protected, ui_state="updating", ui_message="Pi-Code-Sync wird durchgeführt...", progress_step="Services werden neu gestartet...", progress_percent=90)
        for service in ["wage-pi-oled", "wage-pi-leds"]:
            _run(["sudo", "systemctl", "restart", service], timeout=SYSTEMCTL_TIMEOUT_SECONDS)
        _persist_update_state("update erfolgreich", local_commit, remote_commit, changed, obsolete, protected, ui_state="success", ui_message="Update abgeschlossen", progress_step="Update abgeschlossen", progress_percent=100)
        _run(["sudo", "systemctl", "restart", "wage-pi-backend"], timeout=SYSTEMCTL_TIMEOUT_SECONDS)
    except Exception as exc:
        _persist_update_state("update fehlgeschlagen", "", "", [], [], sorted(PROTECTED_RUNTIME_FILES), ui_state="error", ui_message=f"Update fehlgeschlagen: {exc}", progress_step="Update fehlgeschlagen", progress_percent=100)


@router.get("/status")
def update_status():
    state = _read_state(["last_update_status", "last_update_at", "last_update_ui_state", "last_update_ui_message", "last_update_progress_step", "last_update_progress_percent"])
    mode = _network_mode()
    if not _has_internet():
        return _response_payload(mode, state, "", "", [], [], sorted(PROTECTED_RUNTIME_FILES), ui_state="blocked", ui_message="Kein Internet verfügbar. Bitte LAN-Kabel einstecken.")
    _ensure_repo_and_git()
    _fetch_origin_target()
    local_commit, remote_commit, changed, obsolete, protected = _calculate_update_status()
    has_sync = bool(changed or obsolete)
    ui_state = "update_available" if has_sync else "no_update"
    ui_message = "Pi-Code-Sync verfügbar" if has_sync else "Keine Pi-Updates verfügbar"
    return _response_payload(mode, state, local_commit, remote_commit, changed, obsolete, protected, ui_state=ui_state, ui_message=ui_message, can_apply=has_sync)


@router.post("/check")
def update_check():
    mode = _require_internet()
    _persist_update_state("prüfe updates...", "", "", [], [], sorted(PROTECTED_RUNTIME_FILES), ui_state="checking", ui_message="Suche nach Updates...", progress_step="Suche nach Updates...", progress_percent=10)
    _ensure_repo_and_git()
    _fetch_origin_target()
    local_commit, remote_commit, changed, obsolete, protected = _calculate_update_status()
    has_sync = bool(changed or obsolete)
    ui_state = "update_available" if has_sync else "no_update"
    ui_message = "Pi-Code-Sync verfügbar" if has_sync else "Keine Pi-Updates verfügbar"
    _persist_update_state(ui_message, local_commit, remote_commit, changed, obsolete, protected, ui_state=ui_state, ui_message=ui_message, progress_step="Prüfung abgeschlossen", progress_percent=100)
    state = _read_state(["last_update_status", "last_update_at", "last_update_progress_step", "last_update_progress_percent"])
    return _response_payload(mode, state, local_commit, remote_commit, changed, obsolete, protected, ui_state=ui_state, ui_message=ui_message, progress_step="Prüfung abgeschlossen", progress_percent=100, can_apply=has_sync)


@router.post("/apply")
def update_apply():
    mode = _require_internet()
    _ensure_repo_and_git()
    _fetch_origin_target()
    local_commit, remote_commit, changed, obsolete, protected = _calculate_update_status()
    has_sync = bool(changed or obsolete)
    if not has_sync:
        _persist_update_state("kein update nötig", local_commit, remote_commit, [], [], protected, ui_state="no_update", ui_message="Keine Pi-Updates verfügbar", progress_step="Prüfung abgeschlossen", progress_percent=100)
        return _response_payload(mode, _read_state(["last_update_status", "last_update_at", "last_update_progress_step", "last_update_progress_percent"]), local_commit, remote_commit, [], [], protected, ui_state="no_update", ui_message="Keine Pi-Updates verfügbar", progress_step="Prüfung abgeschlossen", progress_percent=100, can_apply=False)
    _persist_update_state("installiere update...", local_commit, remote_commit, changed, obsolete, protected, ui_state="updating", ui_message="Pi-Code-Sync wird durchgeführt...", progress_step="Update wird vorbereitet...", progress_percent=5)
    python_bin = str(REPO_PATH / "pi" / ".venv" / "bin" / "python")
    if not Path(python_bin).exists():
        _persist_update_state("update fehlgeschlagen", local_commit, remote_commit, changed, obsolete, protected, ui_state="error", ui_message="venv nicht gefunden – bitte install.sh ausführen", progress_step="Update fehlgeschlagen", progress_percent=100)
        raise HTTPException(status_code=500, detail="venv nicht gefunden – bitte install.sh ausführen")
    subprocess.Popen([python_bin, "-m", "backend.api_update", "--run-update-job"], cwd=str(REPO_PATH / "pi"), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return _response_payload(mode, _read_state(["last_update_status", "last_update_at", "last_update_progress_step", "last_update_progress_percent"]), local_commit, remote_commit, changed, obsolete, protected, ui_state="updating", ui_message="Pi-Code-Sync wird durchgeführt...", progress_step="Update wird vorbereitet...", progress_percent=5, can_apply=False)


if __name__ == "__main__":
    if "--run-update-job" in sys.argv:
        _run_background_update_job()
