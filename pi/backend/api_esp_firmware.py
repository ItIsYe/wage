from __future__ import annotations

import fcntl
import hashlib
import json
import os
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path

import httpx
from fastapi import APIRouter, HTTPException
from fastapi.responses import StreamingResponse

from .config import PI_ROOT
from .database import db_cursor

router = APIRouter(prefix="/api/v1/esp-firmware", tags=["esp-firmware"])

GITHUB_OWNER = os.getenv("WAGE_GITHUB_OWNER", "ItIsYe")
GITHUB_REPO = os.getenv("WAGE_GITHUB_REPO", "wage")
RELEASE_TAG = os.getenv("WAGE_ESP_RELEASE_TAG", "esp32-latest")
GITHUB_API_BASE = "https://api.github.com"
GITHUB_REQUEST_TIMEOUT = 15.0

CACHE_DIR = PI_ROOT / "firmware_cache"
MANIFEST_CACHE_PATH = CACHE_DIR / "manifest.json"
FIRMWARE_CACHE_PATH = CACHE_DIR / "firmware.bin"
# Beschreibt, welche firmware.bin tatsächlich im Cache liegt (Version + Prüfsummen). Das Manifest allein
# reicht nicht: GET /manifest legt es auch ohne Firmware im Cache ab.
FIRMWARE_META_PATH = CACHE_DIR / "firmware_meta.json"
LOCK_PATH = CACHE_DIR / "sync.lock"


def _ensure_cache_dir() -> None:
    CACHE_DIR.mkdir(parents=True, exist_ok=True)


@contextmanager
def _sync_lock():
    """Verhindert parallele Syncs (Cron + Button + ESP, mehrere Worker) auf dieselben Dateien."""
    _ensure_cache_dir()
    with open(LOCK_PATH, "w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            raise HTTPException(status_code=409, detail="Firmware-Sync läuft bereits")
        try:
            yield
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


def _release_url() -> str:
    return f"{GITHUB_API_BASE}/repos/{GITHUB_OWNER}/{GITHUB_REPO}/releases/tags/{RELEASE_TAG}"


def _find_asset(assets: list[dict], name: str) -> dict | None:
    for asset in assets:
        if asset.get("name") == name:
            return asset
    return None


def _fetch_release() -> dict:
    try:
        resp = httpx.get(_release_url(), timeout=GITHUB_REQUEST_TIMEOUT, headers={"Accept": "application/vnd.github+json"})
    except httpx.HTTPError as exc:
        raise HTTPException(status_code=502, detail=f"GitHub nicht erreichbar: {exc}") from exc
    if resp.status_code == 404:
        raise HTTPException(status_code=404, detail=f"Kein Release mit Tag '{RELEASE_TAG}' gefunden")
    if resp.status_code != 200:
        raise HTTPException(status_code=502, detail=f"GitHub API Fehler ({resp.status_code})")
    return resp.json()


def _download_asset(asset: dict, dest: Path) -> None:
    """Lädt in eine Temp-Datei und ersetzt dest atomar; bei Fehlern bleibt keine halbe Datei zurück."""
    url = asset.get("browser_download_url")
    if not url:
        raise HTTPException(status_code=502, detail="Asset-URL fehlt im Release")
    tmp = dest.with_suffix(dest.suffix + f".{os.getpid()}.tmp")
    try:
        with httpx.stream("GET", url, timeout=GITHUB_REQUEST_TIMEOUT, follow_redirects=True) as resp:
            if resp.status_code != 200:
                raise HTTPException(status_code=502, detail=f"Download fehlgeschlagen ({resp.status_code})")
            with open(tmp, "wb") as f:
                for chunk in resp.iter_bytes(chunk_size=65536):
                    f.write(chunk)
        tmp.replace(dest)
    except httpx.HTTPError as exc:
        raise HTTPException(status_code=502, detail=f"Download fehlgeschlagen: {exc}") from exc
    finally:
        tmp.unlink(missing_ok=True)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            digest.update(chunk)
    return digest.hexdigest()


def _read_json(path: Path) -> dict | None:
    try:
        data = json.loads(path.read_text())
    except (json.JSONDecodeError, OSError):
        return None
    return data if isinstance(data, dict) else None


def _record_check(manifest: dict) -> None:
    now = datetime.now(timezone.utc).isoformat()
    with db_cursor() as (_, cur):
        cur.execute(
            "INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)",
            ("esp_firmware_last_check_at", now),
        )
        cur.execute(
            "INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)",
            ("esp_firmware_latest_version", str(manifest.get("version", ""))),
        )


@router.get("/manifest")
def get_manifest(refresh: bool = False):
    """Liefert das Manifest der aktuellsten ESP-Firmware (Version, SHA256, etc.).

    Lädt das Manifest bei Bedarf frisch von GitHub Releases, sonst aus dem lokalen Cache,
    damit nicht bei jedem ESP-Poll ein GitHub-Request nötig ist.
    """
    if not refresh:
        cached = _read_json(MANIFEST_CACHE_PATH)
        if cached is not None:
            return cached

    _ensure_cache_dir()
    release = _fetch_release()
    manifest_asset = _find_asset(release.get("assets", []), "manifest.json")
    if not manifest_asset:
        raise HTTPException(status_code=502, detail="manifest.json fehlt im Release")

    _download_asset(manifest_asset, MANIFEST_CACHE_PATH)
    manifest = _read_json(MANIFEST_CACHE_PATH)
    if manifest is None:
        raise HTTPException(status_code=502, detail="Manifest ungültig")

    _record_check(manifest)
    return manifest


@router.get("/cache")
def cache_status():
    """Welche Firmware liegt tatsächlich im Cache? (für den Cron-Sync)"""
    meta = _read_json(FIRMWARE_META_PATH)
    has_bin = FIRMWARE_CACHE_PATH.exists()
    return {
        "has_firmware": bool(meta and has_bin),
        "version": (meta or {}).get("version", ""),
        "sha256": (meta or {}).get("sha256", ""),
    }


@router.post("/sync")
def sync_firmware():
    """Lädt Manifest + firmware.bin frisch von GitHub Releases, prüft die SHA256-Summe gegen das Manifest
    und legt beides erst nach erfolgreicher Prüfung im Cache ab.

    Wird vom Pi-Webinterface (Button), vom Cron und vom ESP selbst aufgerufen; danach holt der ESP
    /api/v1/esp-firmware/latest.bin vom Pi, ohne selbst GitHub erreichen zu müssen.
    """
    with _sync_lock():
        release = _fetch_release()
        assets = release.get("assets", [])
        manifest_asset = _find_asset(assets, "manifest.json")
        firmware_asset = _find_asset(assets, "firmware.bin")
        if not manifest_asset or not firmware_asset:
            raise HTTPException(status_code=502, detail="Release unvollständig (manifest.json oder firmware.bin fehlt)")

        _ensure_cache_dir()
        new_manifest = CACHE_DIR / "manifest.json.new"
        new_firmware = CACHE_DIR / "firmware.bin.new"
        try:
            _download_asset(manifest_asset, new_manifest)
            _download_asset(firmware_asset, new_firmware)
            manifest = _read_json(new_manifest)
            if manifest is None:
                raise HTTPException(status_code=502, detail="Manifest ungültig")
            sha256 = _sha256(new_firmware)
            expected = str(manifest.get("sha256", "")).strip().lower()
            if expected and expected != sha256:
                raise HTTPException(status_code=502, detail="SHA256 der Firmware passt nicht zum Manifest (Download beschädigt?)")
            # Erst nach erfolgreicher Prüfung austauschen: Firmware und Manifest bleiben zusammenpassend.
            new_firmware.replace(FIRMWARE_CACHE_PATH)
            new_manifest.replace(MANIFEST_CACHE_PATH)
        finally:
            new_manifest.unlink(missing_ok=True)
            new_firmware.unlink(missing_ok=True)

        FIRMWARE_META_PATH.write_text(json.dumps({"version": manifest.get("version", ""), "sha256": sha256}))
        _record_check(manifest)
        return {"ok": True, "manifest": manifest, "sha256": sha256}


@router.get("/latest.bin")
def get_latest_bin():
    """Streamt die zwischengespeicherte Firmware an den ESP32 (zuvor /sync aufrufen)."""
    if not FIRMWARE_CACHE_PATH.exists():
        raise HTTPException(status_code=404, detail="Keine Firmware im Cache – zuerst /sync aufrufen")

    def iterfile():
        with open(FIRMWARE_CACHE_PATH, "rb") as f:
            while chunk := f.read(65536):
                yield chunk

    size = FIRMWARE_CACHE_PATH.stat().st_size
    headers = {"Content-Length": str(size), "Content-Disposition": "attachment; filename=firmware.bin"}
    meta = _read_json(FIRMWARE_META_PATH)
    if meta:
        headers["X-Firmware-SHA256"] = str(meta.get("sha256", ""))
    return StreamingResponse(iterfile(), media_type="application/octet-stream", headers=headers)
