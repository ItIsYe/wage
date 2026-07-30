#!/usr/bin/env python3
"""
wage-pi-screen-save: Schaltet den Pi-Bildschirm (DSI) aus wenn kein Lauf
für X Minuten eingegangen ist. Aktiviert ihn bei neuem Lauf oder Touch wieder.
"""
import os
import sqlite3
import subprocess
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

DB = Path(__file__).resolve().parents[1] / "data" / "wage_pi.sqlite3"
POLL_SECONDS = 10.0

_screen_off = False
_touch_wakeup = False


def _get_config() -> dict:
    try:
        with sqlite3.connect(DB) as c:
            rows = c.execute(
                "SELECT key, value FROM app_state WHERE key IN "
                "('power_save_after_minutes','last_run_received_at')"
            ).fetchall()
            return {r[0]: r[1] for r in rows}
    except Exception:
        return {}


def _is_power_save() -> bool:
    try:
        cfg = _get_config()
        minutes = int(cfg.get("power_save_after_minutes", "1"))
        if minutes == 0:
            return False
        last_run = cfg.get("last_run_received_at", "")
        if not last_run:
            return False
        last = datetime.fromisoformat(last_run)
        elapsed = (datetime.now(timezone.utc) - last).total_seconds() / 60
        return elapsed >= minutes
    except Exception:
        return False


def _screen_off_cmd():
    env = {**os.environ, "DISPLAY": ":0", "XAUTHORITY": f"/home/{os.getenv('USER','pi')}/.Xauthority"}
    try:
        subprocess.run(["vcgencmd", "display_power", "0", "7"],
                       timeout=5, capture_output=True)
    except Exception:
        pass
    try:
        subprocess.run(["xset", "-display", ":0", "dpms", "force", "off"],
                       env=env, timeout=5, capture_output=True)
    except Exception:
        pass


def _screen_on_cmd():
    env = {**os.environ, "DISPLAY": ":0", "XAUTHORITY": f"/home/{os.getenv('USER','pi')}/.Xauthority"}
    try:
        subprocess.run(["vcgencmd", "display_power", "1", "7"],
                       timeout=5, capture_output=True)
    except Exception:
        pass
    try:
        subprocess.run(["xset", "-display", ":0", "dpms", "force", "on"],
                       env=env, timeout=5, capture_output=True)
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

    # Touch-Watcher in Hintergrund-Thread starten
    t = threading.Thread(target=_touch_watcher, daemon=True)
    t.start()

    while True:
        try:
            should_save = _is_power_save()

            # Touch-Event: Bildschirm wecken und Timer zurücksetzen
            if _touch_wakeup:
                _touch_wakeup = False
                if _screen_off:
                    _screen_on_cmd()
                    _screen_off = False
                # last_run_received_at aktualisieren damit Timer zurückgesetzt wird
                try:
                    with sqlite3.connect(DB) as c:
                        now_iso = datetime.now(timezone.utc).isoformat()
                        c.execute(
                            "INSERT INTO app_state(key,value) VALUES('last_run_received_at',?) "
                            "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                            (now_iso,)
                        )
                        c.commit()
                except Exception:
                    pass

            elif should_save and not _screen_off:
                _screen_off_cmd()
                _screen_off = True
            elif not should_save and _screen_off:
                _screen_on_cmd()
                _screen_off = False

        except Exception:
            pass
        time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    main()
