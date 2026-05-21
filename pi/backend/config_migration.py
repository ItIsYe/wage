from __future__ import annotations

import json
from pathlib import Path

from .config_defaults import DEFAULTS
from .database import db_cursor

CONFIG_JSON_PATH = Path(__file__).resolve().parents[1] / "data" / "network_config.json"
_SECRET_KEYS = {"ap_password", "client_password"}


def ensure_config_defaults() -> dict[str, str]:
    """Ensure all known config defaults exist in app_state without overwriting local values."""
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

    _write_public_network_config(cfg)
    return cfg


def _write_public_network_config(cfg: dict[str, str]) -> None:
    CONFIG_JSON_PATH.parent.mkdir(parents=True, exist_ok=True)
    payload = {k: v for k, v in cfg.items() if k not in _SECRET_KEYS}
    payload["ap_password_set"] = bool(cfg.get("ap_password"))
    payload["client_password_set"] = bool(cfg.get("client_password"))
    CONFIG_JSON_PATH.write_text(json.dumps(payload, indent=2), encoding="utf-8")
