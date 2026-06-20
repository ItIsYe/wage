import os
import sqlite3
import time
from datetime import datetime, timezone
from pathlib import Path

DB = Path(__file__).resolve().parents[1] / "data" / "wage_pi.sqlite3"
POLL_SECONDS = 1.0
REINIT_SECONDS = 10.0
LED_COUNT = int(os.getenv("WAGE_PI_LED_COUNT", "8"))
LED_BRIGHTNESS = max(0, min(255, int(os.getenv("WAGE_PI_LED_BRIGHTNESS", "32"))))

try:
    from backend.config import OFFLINE_THRESHOLD_SECONDS
except Exception:
    OFFLINE_THRESHOLD_SECONDS = 45


def set_state(key: str, value: str) -> None:
    try:
        with sqlite3.connect(DB) as c:
            c.execute(
                "INSERT INTO app_state(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                (key, value),
            )
            c.commit()
    except Exception:
        pass


def get_status() -> dict:
    with sqlite3.connect(DB) as c:
        c.row_factory = sqlite3.Row
        app = {r["key"]: r["value"] for r in c.execute("SELECT key,value FROM app_state")}
        last_run = c.execute("SELECT id FROM runs ORDER BY id DESC LIMIT 1").fetchone()
        last_dev = c.execute("SELECT last_seen_at FROM devices ORDER BY last_seen_at DESC LIMIT 1").fetchone()
    return {
        "app": app,
        "last_run_id": int(last_run["id"]) if last_run else None,
        "last_seen": last_dev["last_seen_at"] if last_dev else None,
    }


def is_online(last_seen: str | None) -> bool:
    if not last_seen:
        return False
    try:
        seen = datetime.fromisoformat(last_seen)
        return (datetime.now(timezone.utc) - seen).total_seconds() <= OFFLINE_THRESHOLD_SECONDS
    except Exception:
        return False


def make_strip():
    from rpi_ws281x import Color, PixelStrip

    strip = PixelStrip(LED_COUNT, 18, brightness=LED_BRIGHTNESS)
    strip.begin()
    return strip, Color


def fill(strip, value):
    for i in range(strip.numPixels()):
        strip.setPixelColor(i, value)
    strip.show()


if __name__ == "__main__":
    set_state("led_status", "starting:white")

    strip = None
    Color = None
    last_init_try = 0.0
    last_run_id = None
    blink_until = 0.0
    blink_toggle = False

    while True:
        now = time.time()
        if strip is None and now - last_init_try >= REINIT_SECONDS:
            last_init_try = now
            try:
                strip, Color = make_strip()
                fill(strip, Color(90, 90, 90))
                set_state("led_status", "starting:white")
            except Exception as exc:
                set_state("led_status", f"degraded:{type(exc).__name__}")

        try:
            s = get_status()
            app = s["app"]
            online = is_online(s["last_seen"])
            api_ok = app.get("api_status", "ok") == "ok"
            db_ok = app.get("database_status", "ok") == "ok"

            status = "running:blue" if not s["last_seen"] else ("running:green" if online else "running:yellow")
            if not (api_ok and db_ok):
                status = "error:red"
            if app.get("last_event") == "fatal_error":
                status = "fatal:red_blink"

            new_run_id = s["last_run_id"]
            if last_run_id is not None and new_run_id and new_run_id != last_run_id:
                blink_until = now + 5.0
            last_run_id = new_run_id

            if now < blink_until:
                status = "event:run_received"

            if strip and Color:
                if status == "event:run_received":
                    fill(strip, Color(0, 180, 0))  # grün für 5s
                elif status == "fatal:red_blink":
                    blink_toggle = not blink_toggle
                    fill(strip, Color(150 if blink_toggle else 0, 0, 0))
                elif status == "running:green":
                    fill(strip, Color(0, 120, 0))
                elif status == "running:yellow":
                    fill(strip, Color(120, 120, 0))
                elif status == "running:blue":
                    fill(strip, Color(0, 0, 110))
                elif status == "error:red":
                    fill(strip, Color(150, 0, 0))
                else:
                    fill(strip, Color(20, 20, 20))

            set_state("led_status", status if strip else "degraded:NoHardware")
        except Exception as exc:
            set_state("led_status", f"degraded:{type(exc).__name__}")
            strip = None
            Color = None

        time.sleep(POLL_SECONDS)
