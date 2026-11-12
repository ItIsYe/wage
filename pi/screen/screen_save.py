#!/usr/bin/env python3
"""
wage-pi-screen-save: Schaltet den Pi-Bildschirm (DSI) aus, wenn X Minuten weder ein Lauf
eingegangen ist noch der Bildschirm bedient wurde. Aktiviert ihn bei neuem Lauf oder Touch wieder.
Nach auto_shutdown_after_minutes (Standard 10, 0 = aus) faehrt der Pi herunter, ausser im Debug-Modus.

Bedienung (Touch, Dienststart) wird als last_activity_at gespeichert, Laeufe als last_run_received_at
(frueher schrieb dieser Dienst die Bedienung in last_run_received_at).
"""
import sqlite3
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

# Der Dienst wird als "python <ordner>/<dienst>.py" gestartet; sys.path[0] ist dann der Unterordner,
# nicht pi/. Ohne das Elternverzeichnis liessen sich backend/ und common/ nicht importieren.
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common.runtime import seconds_since_latest  # noqa: E402

DB = Path(__file__).resolve().parents[1] / "data" / "wage_pi.sqlite3"
BACKLIGHT = Path("/sys/class/backlight/10-0045/brightness")
POLL_SECONDS = 10.0
STARTUP_GRACE_SECONDS = 300  # 5 Minuten nach Boot kein Power-Save
DEFAULT_SHUTDOWN_AFTER_MINUTES = 10

_screen_off = False
_touch_wakeup = False
_start_time = time.monotonic()


def _get_config() -> dict:
    try:
        with sqlite3.connect(DB) as c:
            rows = c.execute(
                "SELECT key, value FROM app_state WHERE key IN "
                "('power_save_after_minutes','auto_shutdown_after_minutes','last_run_received_at','last_activity_at','debug_mode')"
            ).fetchall()
            return {r[0]: r[1] for r in rows}
    except Exception:
        return {}


def _is_debug_mode() -> bool:
    """Debug-Modus: nur Auto-Shutdown deaktiviert."""
    try:
        cfg = _get_config()
        return cfg.get("debug_mode", "0") == "1"
    except Exception:
        return False


def _is_power_save() -> bool:
    if time.monotonic() - _start_time < STARTUP_GRACE_SECONDS:
        return False
    try:
        cfg = _get_config()
        minutes = int(cfg.get("power_save_after_minutes", "1"))
        if minutes == 0:
            return False
        minutes_idle = _inactivity_minutes(cfg)
        return minutes_idle is not None and minutes_idle >= minutes
    except Exception:
        return False


def _inactivity_minutes(cfg: dict | None = None) -> float | None:
    """Minuten seit dem letzten Lauf oder der letzten Bedienung; None, wenn beides unbekannt ist."""
    try:
        cfg = _get_config() if cfg is None else cfg
        age = seconds_since_latest(cfg.get("last_run_received_at"), cfg.get("last_activity_at"))
        return None if age is None else age / 60
    except Exception:
        return None


def _shutdown_after_minutes(cfg: dict) -> int:
    try:
        return int(cfg.get("auto_shutdown_after_minutes", DEFAULT_SHUTDOWN_AFTER_MINUTES))
    except (TypeError, ValueError):
        return DEFAULT_SHUTDOWN_AFTER_MINUTES


def _mark_activity() -> None:
    try:
        with sqlite3.connect(DB) as c:
            c.execute(
                "INSERT INTO app_state(key,value) VALUES('last_activity_at',?) "
                "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                (datetime.now(timezone.utc).isoformat(),)
            )
            c.commit()
    except Exception:
        pass


def _shutdown():
    """Pi herunterfahren — vorher OLED und LEDs ausschalten."""
    import subprocess
    # LEDs ausschalten
    try:
        with sqlite3.connect(DB) as c:
            c.execute(
                "INSERT INTO app_state(key,value) VALUES('led_shutdown','1') "
                "ON CONFLICT(key) DO UPDATE SET value='1'"
            )
            c.commit()
    except Exception:
        pass
    # Die LEDs schaltet der LED-Dienst (root) beim Flag led_shutdown selbst aus. Ein direkter
    # rpi_ws281x-Aufruf von hier (Benutzer wage, System-Python) konnte nie wirken.
    # OLED: Shutdown-Meldung anzeigen
    try:
        from luma.core.interface.serial import i2c
        from luma.oled.device import sh1106, ssd1306
        from PIL import Image, ImageDraw
        serial = i2c(port=1, address=0x3c)
        try:
            dev = ssd1306(serial)
        except Exception:
            dev = sh1106(serial)
        img = Image.new("1", dev.size, 0)
        draw = ImageDraw.Draw(img)
        draw.text((0, 8),  "wage-pi", fill=255)
        draw.text((0, 22), "Wird", fill=255)
        draw.text((0, 34), "ausgeschaltet...", fill=255)
        dev.display(img)
    except Exception:
        pass
    # Backlight aus
    try:
        BACKLIGHT.write_text("0\n")
    except Exception:
        pass
    import time
    time.sleep(1)
    try:
        subprocess.run(["sudo", "shutdown", "-h", "now"], timeout=10)
    except Exception:
        pass


def _screen_off_cmd():
    try:
        BACKLIGHT.write_text("0\n")
    except Exception:
        pass


def _screen_on_cmd():
    try:
        BACKLIGHT.write_text("255\n")
    except Exception:
        pass


def _touch_watcher():
    """Überwacht Touch-Events und setzt _touch_wakeup=True bei Berührung."""
    global _touch_wakeup
    try:
        import evdev
        # Erstes Touch-Device finden
        devices = [evdev.InputDevice(p) for p in evdev.list_devices()]
        touch_dev = None
        for d in devices:
            caps = d.capabilities()
            if evdev.ecodes.EV_ABS in caps:
                touch_dev = d
                break
        if touch_dev is None:
            return
        for event in touch_dev.read_loop():
            if event.type == evdev.ecodes.EV_ABS:
                _touch_wakeup = True
    except Exception:
        pass


def main():
    global _screen_off, _touch_wakeup

    # Beim Start: Bildschirm einschalten, Inaktivitaets-Zeit beginnt jetzt
    _screen_on_cmd()
    _mark_activity()

    # Touch-Watcher in Hintergrund-Thread starten
    t = threading.Thread(target=_touch_watcher, daemon=True)
    t.start()

    while True:
        try:
            cfg = _get_config()
            should_save = _is_power_save()

            # Touch-Event: Bildschirm wecken und Timer zurücksetzen
            if _touch_wakeup:
                _touch_wakeup = False
                if _screen_off:
                    _screen_on_cmd()
                    _screen_off = False
                _mark_activity()  # Timer fuer Power-Save und Auto-Shutdown zuruecksetzen

            elif should_save and not _screen_off:
                _screen_off_cmd()
                _screen_off = True
            elif not should_save and _screen_off:
                _screen_on_cmd()
                _screen_off = False

            # Auto-Shutdown nach X Minuten Inaktivitaet (0 = aus; im Debug-Modus nie)
            shutdown_after = _shutdown_after_minutes(cfg)
            if shutdown_after > 0 and time.monotonic() - _start_time >= STARTUP_GRACE_SECONDS:
                idle = _inactivity_minutes(cfg)
                if idle is not None and idle >= shutdown_after and not _is_debug_mode():
                    _shutdown()

        except Exception:
            pass
        time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    main()
