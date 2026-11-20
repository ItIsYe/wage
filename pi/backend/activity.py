"""Bedienung über das Webinterface als Aktivität vermerken (last_activity_at).

Power-Save und Auto-Shutdown zählen die Zeit seit dem letzten Lauf oder der letzten Bedienung. Bisher
zählte als Bedienung nur der Touchscreen: Wer den Pi per Handy oder Laptop bediente, sah ihn nach
10 Minuten herunterfahren. Diese Middleware vermerkt jede echte Bedienung im Browser.

Nicht als Bedienung gelten: Anfragen der Waage (Läufe, Heartbeat), der Firmware-Sync (Cron/Waage),
statische Dateien und die regelmäßigen Status-Abfragen der Oberfläche. Sonst hielte das offene
Dashboard im Kiosk den Pi für immer wach.
"""
from __future__ import annotations

import asyncio
import sqlite3
import time

from .database import get_connection, utc_now_iso

MIN_WRITE_INTERVAL_SECONDS = 30.0

# Pfade, die nie als Bedienung zählen (Präfixe)
_IGNORED_PREFIXES = (
    "/static/",
    "/api/v1/health",
    "/api/v1/status",            # Polling + SSE-Stream des Dashboards
    "/api/v1/runs/heartbeat",
    "/api/v1/runs/batch",
    "/api/v1/esp-firmware/",     # Cron und Waage
    "/api/v1/system/update/status",
    "/esp-proxy/ota/status",     # Polling der OTA-Seite
)
_DEVICE_EXACT = ("/api/v1/runs",)  # POST /api/v1/runs = Lauf der Waage


def is_user_activity(method: str, path: str) -> bool:
    if path.startswith(_IGNORED_PREFIXES):
        return False
    if method == "POST" and path in _DEVICE_EXACT:
        return False
    if method in ("POST", "PUT", "DELETE", "PATCH"):
        return True
    # GET: nur Seitenaufrufe, keine API-Abfragen (die laufen auch ohne Bedienung im Hintergrund)
    return method == "GET" and not path.startswith("/api/")


def _write_activity() -> None:
    try:
        conn = get_connection()
        try:
            conn.execute(
                "INSERT INTO app_state(key,value) VALUES('last_activity_at',?) "
                "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                (utc_now_iso(),),
            )
            conn.commit()
        finally:
            conn.close()
    except sqlite3.Error:
        pass  # nur ein Hinweis für Power-Save; darf die Anfrage nie scheitern lassen


class ActivityMiddleware:
    def __init__(self, app):
        self.app = app
        self._last_write = 0.0

    async def __call__(self, scope, receive, send):
        if scope.get("type") == "http" and is_user_activity(scope.get("method", ""), scope.get("path", "")):
            now = time.monotonic()
            if now - self._last_write >= MIN_WRITE_INTERVAL_SECONDS:
                self._last_write = now
                await asyncio.to_thread(_write_activity)
        await self.app(scope, receive, send)
