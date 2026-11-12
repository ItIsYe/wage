from __future__ import annotations

from .config_defaults import DEFAULTS
from .database import db_cursor


def ensure_config_defaults() -> dict[str, str]:
    """Stellt sicher, dass alle bekannten Konfigurationswerte in app_state existieren (ueberschreibt nichts)."""
    with db_cursor() as (_, cur):
        keys = tuple(DEFAULTS.keys())
        qmarks = ",".join(["?"] * len(keys))
        rows = cur.execute(f"SELECT key, value FROM app_state WHERE key IN ({qmarks})", keys).fetchall()
        cfg = DEFAULTS.copy()
        for row in rows:
            cfg[row["key"]] = row["value"]
        existing_keys = {row["key"] for row in rows}
        for key, default in DEFAULTS.items():
            if key not in existing_keys:
                cur.execute("INSERT OR IGNORE INTO app_state (key, value) VALUES (?, ?)", (key, default))
    return cfg
