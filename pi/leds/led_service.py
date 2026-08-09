import os
import sqlite3
import time
from datetime import datetime, timezone
from pathlib import Path

DB = Path(__file__).resolve().parents[1] / "data" / "wage_pi.sqlite3"
POLL_SECONDS = 1.0           # DB-Status alle 1s abfragen
POLL_SECONDS_POWERSAVE = 10.0 # DB-Status im Power-Save alle 10s
ANIM_SECONDS = 0.05          # LED-Animation alle 50ms (20 FPS)
ANIM_SECONDS_POWERSAVE = 0.5  # Power-Save: 2 FPS reichen
REINIT_SECONDS = 10.0


def _get_hw_config() -> dict:
    try:
        with sqlite3.connect(DB) as c:
            rows = c.execute(
                "SELECT key, value FROM app_state WHERE key IN "
                "('pi_led_count','pi_led_brightness','pi_led_strip_count','pi_led_strip_pixels','pi_led_strip_sizes','power_save_after_minutes','last_run_received_at','led_brightness_rainbow','led_brightness_pulse_offline','led_brightness_wave','led_brightness_power_save','led_brightness_boot','led_brightness_error')"
            ).fetchall()
            return {r[0]: r[1] for r in rows}
    except Exception:
        return {}


def _get_strip_config() -> list[int]:
    """Gibt Liste mit Pixel-Anzahl pro Streifen zurück."""
    hw = _get_hw_config()
    # Individuelle Größen als kommagetrennte Liste, z.B. "80,80,82,82"
    sizes_str = hw.get("pi_led_strip_sizes", "")
    if sizes_str:
        try:
            sizes = [max(1, int(x.strip())) for x in sizes_str.split(",") if x.strip()]
            if sizes:
                return sizes
        except ValueError:
            pass
    # Fallback: gleichmäßige Aufteilung
    count = int(hw.get("pi_led_strip_count", "4"))
    pixels = int(hw.get("pi_led_strip_pixels", "40"))
    return [pixels] * count


def _get_led_count() -> int:
    return sum(_get_strip_config())


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


def green_wave(strip, Color, strip_sizes: list):
    """Grüne Welle läuft nacheinander durch alle Streifen nach einem Lauf."""
    tail = 8
    speed = 0.02
    start = 0
    for sp in strip_sizes:
        for pos in range(sp + tail):
            for t in range(tail + 1):
                pixel = start + pos - t
                if start <= pixel < start + sp and pixel < strip.numPixels():
                    brightness = int(180 * (1 - t / tail))
                    strip.setPixelColor(pixel, Color(0, brightness, 0))
            clear_pos = start + pos - tail - 1
            if start <= clear_pos < start + sp and clear_pos < strip.numPixels():
                strip.setPixelColor(clear_pos, Color(0, 0, 0))
            strip.show()
            time.sleep(speed)
        for i in range(start, min(start + sp, strip.numPixels())):
            strip.setPixelColor(i, Color(0, 0, 0))
        strip.show()
        start += sp


def boot_sequence(strip, Color):
    """Langsames Hochdimmen beim Start: 0 -> volle Helligkeit in 2 Sekunden."""
    steps = 20
    for i in range(steps + 1):
        brightness = int(255 * i / steps)
        for j in range(strip.numPixels()):
            strip.setPixelColor(j, Color(brightness // 3, brightness // 3, brightness // 3))
        strip.show()
        time.sleep(2.0 / steps)
        time.sleep(2.0 / steps)



# === LED Patterns (nicht-blockierend via State) ===

class RainbowState:
    def __init__(self):
        self.hue = 0.0         # Fließkomma für sanfte Rotation
        self.brightness = 0.0  # Sanfter Aufbau statt reveal (kein Blinken)

class PulseState:
    def __init__(self, color_fn): self.phase = 0.0; self.color_fn = color_fn


def _hsv_to_rgb(hue_deg: float, brightness: float):
    """Saubere HSV->RGB Konvertierung, voller Farbkreis 0-360°."""
    h = (hue_deg % 360) / 60.0
    i = int(h)
    f = h - i
    v = brightness
    q = brightness * (1.0 - f)
    t = brightness * f
    z = 0.0
    if i == 0: return v, t, z
    if i == 1: return q, v, z
    if i == 2: return z, v, t
    if i == 3: return z, q, v
    if i == 4: return t, z, v
    return v, z, q


def rainbow_tick(strip, Color, state: RainbowState, total_pixels: int, max_brightness: int = 80):
    """Flüssiger Regenbogen: voller Farbkreis, sanfter Aufbau, kein Blinken."""
    import math

    # Sanfter Aufbau der Helligkeit beim Start (kein Blinken)
    if state.brightness < 1.0:
        state.brightness = min(1.0, state.brightness + 0.02)

    n = min(total_pixels, strip.numPixels())
    mb = max_brightness * state.brightness

    try:
        import numpy as np
        idx = np.arange(n, dtype=np.float32)
        # Jeden Pixel um gleichen Anteil des Farbkreises versetzt
        hue_deg = (state.hue + idx * 360.0 / n) % 360.0
        # Sinusförmige Helligkeitswelle für organischen Effekt
        bri = mb * (0.75 + 0.25 * np.sin(2 * math.pi * (idx / n - state.hue / 360.0)))

        h = hue_deg / 60.0
        i6 = h.astype(np.int32) % 6
        f = (h - i6.astype(np.float32)).astype(np.float32)
        v = bri.astype(np.float32)
        q = (v * (1.0 - f))
        t = (v * f)
        z = np.zeros(n, np.float32)

        r = np.select([i6==0,i6==1,i6==2,i6==3,i6==4,i6==5],[v,q,z,z,t,v]).clip(0,255).astype(np.uint8)
        g = np.select([i6==0,i6==1,i6==2,i6==3,i6==4,i6==5],[t,v,v,q,z,z]).clip(0,255).astype(np.uint8)
        b = np.select([i6==0,i6==1,i6==2,i6==3,i6==4,i6==5],[z,z,t,v,v,q]).clip(0,255).astype(np.uint8)

        for i in range(n):
            strip.setPixelColor(i, Color(int(r[i]), int(g[i]), int(b[i])))

    except ImportError:
        # Fallback ohne numpy
        for i in range(n):
            hue_deg = (state.hue + i * 360.0 / n) % 360.0
            bri = mb * (0.75 + 0.25 * math.sin(2 * math.pi * (i / n - state.hue / 360.0)))
            rf, gf, bf = _hsv_to_rgb(hue_deg, bri)
            strip.setPixelColor(i, Color(int(rf), int(gf), int(bf)))

    strip.show()
    # Langsame Rotation: 1° pro Frame bei 20 FPS = 20°/s = 18s für volle Runde
    state.hue = (state.hue + 1.0) % 360.0


def pulse_tick(strip, Color, state: PulseState, total_pixels: int, max_brightness: int = 80):
    """Pulsen/Atmen: Helligkeit sinusförmig."""
    import math
    brightness = int((math.sin(state.phase) + 1) / 2 * max_brightness + max(1, max_brightness // 8))
    r, g, b = state.color_fn(brightness)
    for i in range(total_pixels):
        if i < strip.numPixels():
            strip.setPixelColor(i, Color(r, g, b))
    strip.show()
    state.phase = (state.phase + 0.08) % (2 * math.pi)

def _get_led_brightness_for(key: str, default: int) -> int:
    """Liest Pattern-spezifische Helligkeit aus der DB."""
    hw = _get_hw_config()
    return max(0, min(255, int(hw.get(key, str(default)))))



def fill(strip, value):
    for i in range(strip.numPixels()):
        strip.setPixelColor(i, value)
    strip.show()


def _strip_start(strip_sizes: list, strip_index: int) -> int:
    return sum(strip_sizes[:strip_index])


def fill_strip(strip, strip_index: int, value, strip_sizes: list):
    """Einen einzelnen Streifen füllen."""
    start = _strip_start(strip_sizes, strip_index)
    end = start + strip_sizes[strip_index]
    for i in range(start, min(end, strip.numPixels())):
        strip.setPixelColor(i, value)
    strip.show()


def fill_strips(strip, values: list, strip_sizes: list):
    """Jeden Streifen mit eigenem Wert füllen. values = Liste von Colors."""
    start = 0
    for idx, (color, sp) in enumerate(zip(values, strip_sizes)):
        for i in range(start, min(start + sp, strip.numPixels())):
            strip.setPixelColor(i, color)
        start += sp
    strip.show()


def set_pixel(strip, strip_index: int, pixel_index: int, value, strip_sizes: list):
    """Einzelnen Pixel auf einem Streifen setzen."""
    pos = _strip_start(strip_sizes, strip_index) + pixel_index
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
    last_db_poll = 0.0
    status = "running:blue"
    online = False
    _in_power_save = False
    rainbow_state = RainbowState()
    pulse_online_state = PulseState(lambda b: (0, b, 0))       # grün pulsend (online, Standby)
    pulse_offline_state = PulseState(lambda b: (b, b//2, 0))   # gelb pulsend (offline)

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

        # DB-Status alle POLL_SECONDS abfragen
        _poll_interval = POLL_SECONDS_POWERSAVE if _is_power_save() else POLL_SECONDS
        if now - last_db_poll >= _poll_interval:
            last_db_poll = now
            try:
                # Shutdown-Signal prüfen
                with __import__('sqlite3').connect(DB) as _c:
                    _sd = _c.execute("SELECT value FROM app_state WHERE key='led_shutdown'").fetchone()
                    if _sd and _sd[0] == '1':
                        if strip and Color:
                            fill(strip, Color(0, 0, 0))
                        break
                s = get_status()
                app = s["app"]
                online = is_online(s["last_seen"])
                api_ok = app.get("api_status", "ok") == "ok"
                db_ok = app.get("database_status", "ok") == "ok"

                current_status = "running:blue" if not s["last_seen"] else ("running:green" if online else "running:yellow")
                if not (api_ok and db_ok):
                    current_status = "error:red"
                if app.get("last_event") == "fatal_error":
                    current_status = "fatal:red_blink"

                new_run_id = s["last_run_id"]
                if last_run_id is not None and new_run_id and new_run_id != last_run_id:
                    blink_until = now + 5.0
                    if strip and Color:
                        green_wave(strip, Color, _get_strip_config())
                last_run_id = new_run_id

                if now < blink_until:
                    current_status = "event:run_received"
                status = current_status
            except Exception as exc:
                set_state("led_status", f"degraded:{type(exc).__name__}")
                strip = None
                Color = None

        # LED-Animation bei jedem ANIM_SECONDS Tick rendern
        if strip and Color:
            try:
                total = strip.numPixels()
                power_save = _is_power_save()
                if power_save and now >= blink_until:
                    pulse_tick(strip, Color, PulseState(lambda b: (b//8, b//8, b//8)), total, _get_led_brightness_for("led_brightness_power_save", 12))
                    set_state("led_status", "power_save")
                elif status == "event:run_received":
                    pass
                elif status == "fatal:red_blink":
                    blink_toggle = not blink_toggle
                    fill(strip, Color(_get_led_brightness_for("led_brightness_error", 100) if blink_toggle else 0, 0, 0))
                elif status == "error:red":
                    fill(strip, Color(_get_led_brightness_for("led_brightness_error", 100), 0, 0))
                elif not online:
                    pulse_tick(strip, Color, pulse_offline_state, total, _get_led_brightness_for("led_brightness_pulse_offline", 60))
                else:
                    rainbow_tick(strip, Color, rainbow_state, total, _get_led_brightness_for("led_brightness_rainbow", 80))
                if not power_save or now < blink_until:
                    set_state("led_status", status)
            except Exception as exc:
                set_state("led_status", f"degraded:{type(exc).__name__}")
                strip = None
                Color = None

        # Im Power-Save: langsamer schlafen spart CPU
        _in_power_save = (status == "power_save" or _is_power_save())
        time.sleep(ANIM_SECONDS_POWERSAVE if _in_power_save else ANIM_SECONDS)
