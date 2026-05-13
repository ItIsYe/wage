import sqlite3
from contextlib import contextmanager
from datetime import datetime, timezone

from .config import DB_PATH


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def get_connection() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db() -> None:
    with get_connection() as conn:
        conn.executescript(
            """
            CREATE TABLE IF NOT EXISTS persons (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT NOT NULL UNIQUE,
                created_at TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS devices (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL UNIQUE,
                firmware_version TEXT,
                last_seen_at TEXT,
                last_boot_id TEXT,
                last_queue_depth INTEGER
            );

            CREATE TABLE IF NOT EXISTS runs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                boot_id TEXT NOT NULL,
                run_number INTEGER NOT NULL,
                event_id TEXT NOT NULL,
                time_ms INTEGER NOT NULL,
                start_weight_g REAL NOT NULL,
                status TEXT NOT NULL,
                firmware_version TEXT NOT NULL,
                queue_depth INTEGER,
                received_at TEXT NOT NULL,
                person_id INTEGER,
                person_name TEXT,
                note TEXT DEFAULT '',
                UNIQUE(device_id, event_id)
            );

            CREATE TABLE IF NOT EXISTS app_state (
                key TEXT PRIMARY KEY,
                value TEXT
            );
            """
        )
        conn.execute(
            "INSERT OR IGNORE INTO persons (id, name, created_at) VALUES (1, 'Unbekannt', ?)",
            (utc_now_iso(),),
        )
        conn.execute(
            "INSERT OR IGNORE INTO app_state (key, value) VALUES ('active_person_id', '1')"
        )
        conn.execute(
            "INSERT OR IGNORE INTO app_state (key, value) VALUES ('led_status', 'unknown')"
        )
        conn.execute(
            "INSERT OR IGNORE INTO app_state (key, value) VALUES ('oled_status', 'unknown')"
        )
        conn.commit()


@contextmanager
def db_cursor():
    conn = get_connection()
    try:
        yield conn, conn.cursor()
        conn.commit()
    finally:
        conn.close()
