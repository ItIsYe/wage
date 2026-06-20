import sqlite3
from contextlib import contextmanager
from datetime import datetime, timezone

from .config import DB_PATH


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def get_connection() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH, timeout=15, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")
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
                last_boot_id INTEGER,
                last_queue_depth INTEGER,
                last_ip TEXT
            );

            CREATE TABLE IF NOT EXISTS runs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                boot_id TEXT NOT NULL,
                run_number INTEGER NOT NULL,
                event_id TEXT NOT NULL,
                time_ms INTEGER NOT NULL,
                start_weight_g REAL NOT NULL,
                min_weight_g REAL,
                start_drop_threshold_g REAL,
                stop_rise_threshold_g REAL,
                status TEXT NOT NULL,
                firmware_version TEXT NOT NULL,
                queue_depth INTEGER,
                received_at TEXT NOT NULL,
                person_id INTEGER,
                person_name TEXT,
                note TEXT DEFAULT '',
                UNIQUE(device_id, event_id),
                FOREIGN KEY(person_id) REFERENCES persons(id) ON DELETE SET NULL
            );

            CREATE TABLE IF NOT EXISTS app_state (
                key TEXT PRIMARY KEY,
                value TEXT
            );

            CREATE INDEX IF NOT EXISTS idx_runs_received_at ON runs(received_at DESC);
            CREATE INDEX IF NOT EXISTS idx_runs_person_id ON runs(person_id);
            CREATE INDEX IF NOT EXISTS idx_runs_status ON runs(status);
            CREATE INDEX IF NOT EXISTS idx_runs_boot_run ON runs(device_id, boot_id, run_number);
            """
        )
        # Migration: neue Spalten für bestehende DBs ergänzen
        existing_cols = {row[0] for row in conn.execute("PRAGMA table_info(runs)").fetchall()}
        for col, typedef in [
            ("min_weight_g", "REAL"),
            ("start_drop_threshold_g", "REAL"),
            ("stop_rise_threshold_g", "REAL"),
        ]:
            if col not in existing_cols:
                try:
                    conn.execute(f"ALTER TABLE runs ADD COLUMN {col} {typedef}")
                except sqlite3.OperationalError:
                    pass
        dev_cols = {row[0] for row in conn.execute("PRAGMA table_info(devices)").fetchall()}
        if "last_ip" not in dev_cols:
            try:
                conn.execute("ALTER TABLE devices ADD COLUMN last_ip TEXT")
            except sqlite3.OperationalError:
                pass
        conn.execute("INSERT OR IGNORE INTO persons (id, name, created_at) VALUES (1, 'Unbekannt', ?)", (utc_now_iso(),))
        conn.execute("INSERT OR IGNORE INTO app_state (key, value) VALUES ('active_person_id', '1')")
        conn.execute("INSERT OR IGNORE INTO app_state (key, value) VALUES ('led_status', 'init')")
        conn.execute("INSERT OR IGNORE INTO app_state (key, value) VALUES ('oled_status', 'init')")
        conn.execute("INSERT OR IGNORE INTO app_state (key, value) VALUES ('last_event', 'startup')")


@contextmanager
def db_cursor():
    conn = get_connection()
    cur = conn.cursor()
    try:
        yield conn, cur
        conn.commit()
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()
