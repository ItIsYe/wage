import os
import sqlite3
import time
from datetime import datetime, timezone
from pathlib import Path

DB = Path(__file__).resolve().parents[1] / "data" / "wage_pi.sqlite3"
POLL_SECONDS = 1.0
REINIT_SECONDS = 10.0


def _get_hw_config() -> dict:
    try:
        with sqlite3.connect(DB) as c:
            rows = c.execute(
                "SELECT key, value FROM app_state WHERE key IN "
                "('pi_led_count','pi_led_brightness','pi_led_strip_count','pi_led_strip_pixels',"
                "'power_save_after_minutes','last_run_received_at')"
            ).fetchall()
            return {r[0]: r[1] for r in rows}
    except Exception:
        return {}


def _get_strip_config() -> tuple[int, int]:
    """Gibt (strip_count, pixels_per_strip) zurück."""
    hw = _get_hw_config()
    strip_count = int(hw.get("pi_led_strip_count", "4"))
    strip_pixels = int(hw.get("pi_led_strip_pixels", "40"))
    return strip_count, strip_pixels


def _get_led_count() -> int:
    strip_count, strip_pixels = _get_strip_config()
    return strip_count * strip_pixels


def _get_led_brightness() -> int:
    hw = _get_hw_config()
    return max(0, min(255, int(hw.get("pi_led_brightness", os.getenv("WAGE_PI_LED_BRIGHTNESS", "32")))))


_service_start = time.time()
STARTUP_GRACE_SECONDS = 120


def _is_power_save() -> bool:
    if time.time() - _service_start < STARTUP_GRACE_SECONDS:
        return False
    try:
        hw = _get_hw_config()
        minutes = int(hw.get("power_save_after_minutes", "1"))
        if minutes == 0:
            return False
        last_run = hw.get("last_run_received_at", "")
        if not last_run:
            return False  # Noch kein Lauf -> kein Power-Save
        from datetime import datetime, timezone
        last = datetime.fromisoformat(last_run)
        elapsed = (datetime.now(timezone.utc) - last).total_seconds() / 60
        return elapsed >= minutes
    except Exception:
        return False

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

    strip = PixelStrip(_get_led_count(), 18, brightness=_get_led_brightness())
    strip.begin()
    return strip, Color


def green_wave(strip, Color, strip_count: int, strip_pixels: int):
    """Grüne Welle läuft nacheinander durch alle Streifen nach einem Lauf."""
    tail = 8  # Schweif-Länge
    speed = 0.02  # Sekunden pro Schritt

    for s in range(strip_count):
        start = s * strip_pixels
        # Streifen davor dunkel lassen
        for pos in range(strip_pixels + tail):
            # Pixel vor dem Kopf aufleuchten, Schweif abdimmen
            for t in range(tail + 1):
                pixel = start + pos - t
                if 0 <= pixel < start + strip_pixels and pixel < strip.numPixels():
                    brightness = int(180 * (1 - t / tail))
                    strip.setPixelColor(pixel, Color(0, brightness, 0))
            # Hinter dem Schweif löschen
            clear_pos = start + pos - tail - 1
            if start <= clear_pos < start + strip_pixels and clear_pos < strip.numPixels():
                strip.setPixelColor(clear_pos, Color(0, 0, 0))
            strip.show()
            time.sleep(speed)
        # Streifen nach Welle löschen
        for i in range(start, min(start + strip_pixels, strip.numPixels())):
            strip.setPixelColor(i, Color(0, 0, 0))
        strip.show()
    """Langsames Hochdimmen beim Start: 0 -> volle Helligkeit in 2 Sekunden."""
    steps = 20
    for i in range(steps + 1):
        brightness = int(255 * i / steps)
        for j in range(strip.numPixels()):
            strip.setPixelColor(j, Color(brightness // 3, brightness // 3, brightness // 3))
        strip.show()
        time.sleep(2.0 / steps)


def fill(strip, value):
    for i in range(strip.numPixels()):
        strip.setPixelColor(i, value)
    strip.show()


def fill_strip(strip, strip_index: int, value, strip_pixels: int):
    """Einen einzelnen Streifen füllen."""
    start = strip_index * strip_pixels
    end = start + strip_pixels
    for i in range(start, min(end, strip.numPixels())):
        strip.setPixelColor(i, value)
    strip.show()


def fill_strips(strip, values: list, strip_pixels: int):
    """Jeden Streifen mit eigenem Wert füllen. values = Liste von Colors."""
    for idx, color in enumerate(values):
        start = idx * strip_pixels
        end = start + strip_pixels
        for i in range(start, min(end, strip.numPixels())):
            strip.setPixelColor(i, color)
    strip.show()


def set_pixel(strip, strip_index: int, pixel_index: int, value, strip_pixels: int):
    """Einzelnen Pixel auf einem Streifen setzen."""
    pos = strip_index * strip_pixels + pixel_index
    if pos < strip.numPixels():
        strip.setPixelColor(pos, value)
        strip.show()


if __name__ == "__main__":
    set_state("led_status", "starting:white")

    strip = None
    Color = None
    last_init_try = 0.0
    last_led_count = None
    last_led_brightness = None
    last_run_id = None
    blink_until = 0.0
    blink_toggle = False

    while True:
        now = time.time()
        if strip is None and now - last_init_try >= REINIT_SECONDS:
            last_init_try = now
            try:
                strip, Color = make_strip()
                last_led_count = _get_led_count()
                last_led_brightness = _get_led_brightness()
                boot_sequence(strip, Color)
                set_state("led_status", "starting:white")
            except Exception as exc:
                set_state("led_status", f"degraded:{type(exc).__name__}")

        # Konfigurationsänderung erkennen -> Strip neu initialisieren
        if strip is not None:
            current_count = _get_led_count()
            current_brightness = _get_led_brightness()
            if current_count != last_led_count or current_brightness != last_led_brightness:
                try:
                    fill(strip, Color(0, 0, 0))
                    strip = None
                    last_init_try = 0.0
                except Exception:
                    strip = None
                continue

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
                # Grüne Welle sofort auslösen
                if strip and Color:
                    sc, sp = _get_strip_config()
                    green_wave(strip, Color, sc, sp)
            last_run_id = new_run_id

            if now < blink_until:
                status = "event:run_received"

            if strip and Color:
                power_save = _is_power_save()
                if power_save and now >= blink_until:
                    # Energiesparmodus: sehr gedimmt (ca. 5% Helligkeit)
                    fill(strip, Color(6, 6, 6))
                    set_state("led_status", "power_save")
                elif status == "event:run_received":
                    pass  # Welle wurde bereits beim Erkennen abgespielt
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

            if not (strip and Color and _is_power_save() and now >= blink_until):
                set_state("led_status", status if strip else "degraded:NoHardware")
        except Exception as exc:
            set_state("led_status", f"degraded:{type(exc).__name__}")
            strip = None
            Color = None

        time.sleep(POLL_SECONDS)
