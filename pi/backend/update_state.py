"""Update-Zustand in der Datenbank (app_state), gemeinsam genutzt von der API und von scripts/update.sh.

update.sh läuft als eigener Prozess und meldet Fortschritt, Erfolg und Fehler selbst:
    python -m backend.update_state <ui_state> <status> <ui_message> <progress_step> <progress_percent>
(aus dem Ordner pi/ aufrufen). Schlägt das Schreiben fehl, bricht das Update NICHT ab (Exit-Code 0).
"""
from __future__ import annotations

import sqlite3
import sys
from datetime import datetime, timezone

from .config import DB_PATH


def write_state(entries: dict[str, str]) -> str:
    """Schreibt die Einträge in app_state und setzt last_update_at. Gibt den Zeitstempel zurück."""
    now = datetime.now(timezone.utc).isoformat()
    values = {**entries, "last_update_at": now}
    conn = sqlite3.connect(DB_PATH, timeout=15)
    try:
        with conn:
            for key, value in values.items():
                conn.execute("INSERT OR REPLACE INTO app_state (key, value) VALUES (?, ?)", (key, value))
    finally:
        conn.close()
    return now


def progress_entries(ui_state: str, status: str, message: str, step: str, percent: int) -> dict[str, str]:
    return {
        "last_update_ui_state": ui_state,
        "last_update_status": status,
        "last_update_ui_message": message,
        "last_update_progress_step": step,
        "last_update_progress_percent": str(percent),
    }


def main(argv: list[str]) -> int:
    if len(argv) != 6:
        print("Aufruf: python -m backend.update_state <ui_state> <status> <ui_message> <progress_step> <progress_percent>", file=sys.stderr)
        return 2
    ui_state, status, message, step, percent = argv[1:6]
    try:
        write_state(progress_entries(ui_state, status, message, step, int(percent)))
    except (sqlite3.Error, ValueError, OSError) as exc:
        print(f"update_state: {exc}", file=sys.stderr)
    return 0  # das Update selbst soll nie an der Statusmeldung scheitern


if __name__ == "__main__":
    sys.exit(main(sys.argv))
