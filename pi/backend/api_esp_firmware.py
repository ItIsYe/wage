from __future__ import annotations

import json
import os
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
CACHE_DIR.mkdir(parents=True, exist_ok=True)
MANIFEST_CACHE_PATH = CACHE_DIR / "manifest.json"
FIRMWARE_CACHE_PATH = CACHE_DIR / "firmware.bin"


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
    url = asset.get("browser_download_url")
    if not url:
        raise HTTPException(status_code=502, detail="Asset-URL fehlt im Release")
    try:
        with httpx.stream("GET", url, timeout=GITHUB_REQUEST_TIMEOUT, follow_redirects=True) as resp:
            if resp.status_code != 200:
                raise HTTPException(status_code=502, detail=f"Download fehlgeschlagen ({resp.status_code})")
            tmp = dest.with_suffix(dest.suffix + ".tmp")
            with open(tmp, "wb") as f:
                for chunk in resp.iter_bytes(chunk_size=65536):
                    f.write(chunk)
            tmp.replace(dest)
    except httpx.HTTPError as exc:
        raise HTTPException(status_code=502, detail=f"Download fehlgeschlagen: {exc}") from exc


def _record_check(manifest: dict) -> None:
    now = datetime.now(timezone.utc).isoformat()
    with db_cursor() as (_, cur):
        cur.execute(
            "INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)",
            ("esp_firmware_last_check_at", now),
        )
        cur.execute(
            "INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)",
            ("esp_firmware_latest_version", manifest.get("version", "")),
        )


@router.get("/manifest")
def get_manifest(refresh: bool = False):
    """Liefert das Manifest der aktuellsten ESP-Firmware (Version, SHA256, etc.).

    Lädt das Manifest bei Bedarf frisch von GitHub Releases, sonst aus dem
    lokalen Cache, damit nicht bei jedem ESP-Poll ein GitHub-Request nötig ist.
    """
    if not refresh and MANIFEST_CACHE_PATH.exists():
        try:
            cached = json.loads(MANIFEST_CACHE_PATH.read_text())
            return cached
        except (json.JSONDecodeError, OSError):
            pass

    release = _fetch_release()
    assets = release.get("assets", [])
    manifest_asset = _find_asset(assets, "manifest.json")
    if not manifest_asset:
        raise HTTPException(status_code=502, detail="manifest.json fehlt im Release")

    _download_asset(manifest_asset, MANIFEST_CACHE_PATH)
    try:
        manifest = json.loads(MANIFEST_CACHE_PATH.read_text())
    except (json.JSONDecodeError, OSError) as exc:
        raise HTTPException(status_code=502, detail=f"Manifest ungültig: {exc}") from exc

    _record_check(manifest)
    return manifest


@router.post("/sync")
def sync_firmware():
    """Lädt Manifest + firmware.bin frisch von GitHub Releases in den lokalen Cache.

    Wird vom Pi-Webinterface aufgerufen (Button), danach kann der ESP
    /api/v1/esp-firmware/latest.bin über den Pi abrufen ohne dass der ESP
    selbst GitHub erreichen muss.
    """
    release = _fetch_release()
    assets = release.get("assets", [])

    manifest_asset = _find_asset(assets, "manifest.json")
    firmware_asset = _find_asset(assets, "firmware.bin")
    if not manifest_asset or not firmware_asset:
        raise HTTPException(status_code=502, detail="Release unvollständig (manifest.json oder firmware.bin fehlt)")

    _download_asset(manifest_asset, MANIFEST_CACHE_PATH)
    _download_asset(firmware_asset, FIRMWARE_CACHE_PATH)

    try:
        manifest = json.loads(MANIFEST_CACHE_PATH.read_text())
    except (json.JSONDecodeError, OSError) as exc:
        raise HTTPException(status_code=502, detail=f"Manifest ungültig: {exc}") from exc

    _record_check(manifest)
    return {"ok": True, "manifest": manifest}


@router.get("/latest.bin")
def get_latest_bin():
    """Streamt die zwischengespeicherte Firmware an den ESP32.

    Erwartet, dass vorher /sync aufgerufen wurde (Button im Webinterface).
    So muss der ESP selbst nie GitHub erreichen, nur den Pi im eigenen Netz.
    """
    if not FIRMWARE_CACHE_PATH.exists():
        raise HTTPException(status_code=404, detail="Keine Firmware im Cache – zuerst /sync aufrufen")

    def iterfile():
        with open(FIRMWARE_CACHE_PATH, "rb") as f:
            while chunk := f.read(65536):
                yield chunk

    size = FIRMWARE_CACHE_PATH.stat().st_size
    return StreamingResponse(
        iterfile(),
        media_type="application/octet-stream",
        headers={"Content-Length": str(size), "Content-Disposition": "attachment; filename=firmware.bin"},
    )
