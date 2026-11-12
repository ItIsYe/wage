import os
import sqlite3
import time
from pathlib import Path

# Der Dienst wird als "python <ordner>/<dienst>.py" gestartet; sys.path[0] ist dann der Unterordner,
# nicht pi/. Ohne das Elternverzeichnis liessen sich backend/ und common/ nicht importieren.
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common.runtime import seconds_since, seconds_since_latest  # noqa: E402

DB = Path(__file__).resolve().parents[1] / "data" / "wage_pi.sqlite3"
POLL_SECONDS = 1.0           # DB-Status alle 1s abfragen
POLL_SECONDS_POWERSAVE = 10.0 # DB-Status im Power-Save alle 10s
ANIM_SECONDS = 0.08          # LED-Animation alle 80ms (12 FPS) - stabiler bei 322 Pixeln
ANIM_SECONDS_POWERSAVE = 0.5  # Power-Save: 2 FPS reichen
REINIT_SECONDS = 10.0


# Die Hauptschleife fragt die Konfiguration mehrfach pro Animations-Tick (80 ms) ab.
# Ohne Cache waren das rund 50 DB-Verbindungen pro Sekunde. Eine Sekunde Verzögerung
# bei Konfigurationsänderungen ist unkritisch.
_HW_CONFIG_TTL_SECONDS = 1.0
_hw_config_cache: dict = {}
_hw_config_cache_at = None


def _get_hw_config() -> dict:
    global _hw_config_cache, _hw_config_cache_at
    now = time.monotonic()
    if _hw_config_cache_at is not None and (now - _hw_config_cache_at) < _HW_CONFIG_TTL_SECONDS:
        return _hw_config_cache
    try:
        with sqlite3.connect(DB) as c:
            rows = c.execute(
                "SELECT key, value FROM app_state WHERE key IN "
                "('pi_led_count','pi_led_brightness','pi_led_strip_count','pi_led_strip_pixels','pi_led_strip_sizes','power_save_after_minutes','last_run_received_at','last_activity_at','led_brightness_rainbow','led_brightness_pulse_offline','led_brightness_wave','led_brightness_power_save','led_brightness_boot','led_brightness_error')"
            ).fetchall()
        _hw_config_cache = {r[0]: r[1] for r in rows}
    except Exception:
        # DB kurz nicht lesbar (z.B. gesperrt): letzten bekannten Stand behalten statt
        # auf Defaults zurückzufallen (das würde den Strip mit falscher Pixelzahl neu
        # initialisieren). Gibt es noch keinen Stand, greifen die Defaults der Aufrufer.
        pass
    _hw_config_cache_at = now
    return _hw_config_cache


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
        # Inaktivitaet = Zeit seit dem letzten Lauf ODER der letzten Bedienung (Touch, Start)
        age = seconds_since_latest(hw.get("last_run_received_at"), hw.get("last_activity_at"))
        if age is None:
            # Noch nie ein Lauf/eine Bedienung -> Service-Start als Referenz nehmen
            age = time.time() - _service_start
        return age / 60 >= minutes
    except Exception:
        return False

try:
    from backend.config import OFFLINE_THRESHOLD_SECONDS
except Exception:
    OFFLINE_THRESHOLD_SECONDS = 90


# set_state() wird in jedem Animations-Tick aufgerufen, der Wert ändert sich aber fast
# nie. Ohne Vergleich waren das rund 12 Schreib-Commits pro Sekunde (ca. 1 Mio. pro Tag)
# auf die SD-Karte. Geschrieben wird nur bei Änderung, sonst höchstens einmal pro Minute.
_STATE_REFRESH_SECONDS = 60.0
_last_written_state: dict = {}  # key -> (value, Zeitpunkt des letzten erfolgreichen Schreibens)


def set_state(key: str, value: str) -> None:
    now = time.monotonic()
    last = _last_written_state.get(key)
    if last is not None and last[0] == value and (now - last[1]) < _STATE_REFRESH_SECONDS:
        return
    try:
        with sqlite3.connect(DB) as c:
            c.execute(
                "INSERT INTO app_state(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                (key, value),
            )
            c.commit()
        _last_written_state[key] = (value, now)
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
    age = seconds_since(last_seen)
    return age is not None and age <= OFFLINE_THRESHOLD_SECONDS


# Aktuell am Strip eingestellte Helligkeit (rpi_ws281x skaliert beim Senden; ein Wechsel
# kostet nichts, wird aber nur bei Aenderung gesetzt).
_applied_brightness = None


def apply_brightness(strip, value: int) -> None:
    """Strip-Helligkeit (0-255) setzen. Die Slider "Regenbogen", "Gruene Welle" und "Boot"
    der LED-Konfiguration legen diese Helligkeit fuer ihr Muster fest; alle anderen
    Muster laufen mit der globalen Helligkeit (Konfiguration -> Pi Hardware)."""
    global _applied_brightness
    value = max(0, min(255, int(value)))
    if value != _applied_brightness:
        strip.setBrightness(value)
        _applied_brightness = value


def make_strip():
    global _applied_brightness
    _applied_brightness = None
    from rpi_ws281x import Color, PixelStrip

    strip = PixelStrip(_get_led_count(), 18, brightness=_get_led_brightness())
    strip.begin()
    _applied_brightness = _get_led_brightness()
    return strip, Color


def green_wave(strip, Color, strip_sizes: list):
    """Grüne Welle läuft nacheinander durch alle Streifen nach einem Lauf."""
    apply_brightness(strip, _get_led_brightness_for("led_brightness_wave", 120))
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
    apply_brightness(strip, _get_led_brightness_for("led_brightness_boot", 40))
    steps = 20
    for i in range(steps + 1):
        brightness = int(255 * i / steps)
        for j in range(strip.numPixels()):
            strip.setPixelColor(j, Color(brightness // 3, brightness // 3, brightness // 3))
        strip.show()
        time.sleep(2.0 / steps)



# === LED Patterns (nicht-blockierend via State) ===

class RainbowState:
    def __init__(self):
        self.offset = 0   # 0-255, Adafruit rainbowCycle-Stil

class PulseState:
    def __init__(self, color_fn): self.phase = 0.0; self.color_fn = color_fn


def _wheel(pos: int) -> tuple:
    """Adafruit wheel() - bewährte stückweise lineare HSV-Näherung (0-255)."""
    pos = pos & 255
    if pos < 85:
        return (pos * 3, 255 - pos * 3, 0)
    if pos < 170:
        pos -= 85
        return (255 - pos * 3, 0, pos * 3)
    pos -= 170
    return (0, pos * 3, 255 - pos * 3)


def rainbow_tick(strip, Color, state: RainbowState, total_pixels: int):
    """Flüssiger Regenbogen. Die Helligkeit kommt über apply_brightness (led_brightness_rainbow)."""
    n = min(total_pixels, strip.numPixels())
    offset = state.offset
    for i in range(n):
        pos = (i * 256 // n + offset) & 255
        r, g, b = _wheel(pos)
        strip.setPixelColor(i, Color(r, g, b))
    strip.show()
    state.offset = (state.offset + 1) & 255


def pulse_tick(strip, Color, state: PulseState, total_pixels: int, max_brightness: int = 80):
    """Pulsen/Atmen: Helligkeit sinusförmig."""
    import math
    brightness = int((math.sin(state.phase) + 1) / 2 * max_brightness + max(1, max_brightness // 8))
    r, g, b = state.color_fn(brightness)
    for i in range(min(total_pixels, strip.numPixels())):
        strip.setPixelColor(i, Color(r, g, b))
    strip.show()
    state.phase = (state.phase + 0.02) % (2 * math.pi)  # Sehr langsam = sehr flüssig

def _get_led_brightness_for(key: str, default: int) -> int:
    """Liest Pattern-spezifische Helligkeit aus der DB."""
    hw = _get_hw_config()
    return max(0, min(255, int(hw.get(key, str(default)))))



def fill(strip, value):
    for i in range(strip.numPixels()):
        strip.setPixelColor(i, value)
    strip.show()


if __name__ == "__main__":
    set_state("led_status", "starting:white")

    strip = None
    Color = None
    last_init_try = 0.0
    last_led_count = None
    last_run_id = None
    blink_until = 0.0
    last_db_poll = 0.0
    status = "running:blue"
    online = False
    twinkle_state = None
    rainbow_state = RainbowState()
    pulse_offline_state = PulseState(lambda b: (b, b, 0))     # reines Gelb

    # led_shutdown Flag beim Start zurücksetzen
    try:
        with __import__('sqlite3').connect(DB) as _c:
            _c.execute("INSERT INTO app_state(key,value) VALUES('led_shutdown','0') "
                      "ON CONFLICT(key) DO UPDATE SET value='0'")
            _c.commit()
    except Exception:
        pass

    while True:
        now = time.time()
        if strip is None and now - last_init_try >= REINIT_SECONDS:
            last_init_try = now
            try:
                strip, Color = make_strip()
                last_led_count = _get_led_count()
                from twinkle import TwinkleState
                twinkle_state = TwinkleState(strip.numPixels())
                boot_sequence(strip, Color)
                set_state("led_status", "starting:white")
            except Exception as exc:
                set_state("led_status", f"degraded:{type(exc).__name__}")

        # Geänderte Pixelzahl -> Strip neu initialisieren. Helligkeitsänderungen braucht es dafür nicht:
        # apply_brightness() setzt sie im nächsten Tick (früher: Neuinitialisierung mit Hochdimmen).
        if strip is not None:
            current_count = _get_led_count()
            if current_count != last_led_count:
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
                online = is_online(s["last_seen"])
                current_status = "running:blue" if not s["last_seen"] else ("running:green" if online else "running:yellow")

                new_run_id = s["last_run_id"]
                if last_run_id is not None and new_run_id and new_run_id != last_run_id:
                    blink_until = now + 5.0
                    if strip and Color:
                        green_wave(strip, Color, _get_strip_config())
                last_run_id = new_run_id

                if now < blink_until:
                    current_status = "event:run_received"
                status = current_status
            except sqlite3.Error:
                # Datenbank nicht lesbar: rot anzeigen (Strip bleibt in Betrieb)
                status = "error:red"
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
                    if twinkle_state is None or twinkle_state.n != total:
                        from twinkle import TwinkleState
                        twinkle_state = TwinkleState(total)
                    apply_brightness(strip, _get_led_brightness())
                    twinkle_state.tick(strip, Color, _get_led_brightness_for("led_brightness_power_save", 12))
                    set_state("led_status", "power_save")
                elif status == "event:run_received":
                    pass
                elif status == "error:red":
                    apply_brightness(strip, _get_led_brightness())
                    fill(strip, Color(_get_led_brightness_for("led_brightness_error", 100), 0, 0))
                elif not online:
                    apply_brightness(strip, _get_led_brightness())
                    pulse_tick(strip, Color, pulse_offline_state, total, _get_led_brightness_for("led_brightness_pulse_offline", 60))
                else:
                    apply_brightness(strip, _get_led_brightness_for("led_brightness_rainbow", 80))
                    rainbow_tick(strip, Color, rainbow_state, total)
                if not power_save or now < blink_until:
                    set_state("led_status", status)
            except Exception as exc:
                set_state("led_status", f"degraded:{type(exc).__name__}")
                strip = None
                Color = None

        # Im Power-Save: langsamer schlafen spart CPU
        time.sleep(ANIM_SECONDS_POWERSAVE if _is_power_save() else ANIM_SECONDS)
