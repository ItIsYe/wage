#!/usr/bin/env python3
"""
wage-pi-screen-save: Schaltet den Pi-Bildschirm (HDMI) aus wenn kein Lauf
für X Minuten eingegangen ist. Aktiviert ihn bei neuem Lauf wieder.
"""
import os
import sqlite3
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path

DB = Path(__file__).resolve().parents[1] / "data" / "wage_pi.sqlite3"
POLL_SECONDS = 10.0

_screen_off = False


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
    """HDMI und Display-Backlight ausschalten."""
    env = {**os.environ, "DISPLAY": ":0", "XAUTHORITY": f"/home/{os.getenv('USER','pi')}/.Xauthority"}
    try:
        subprocess.run(["xset", "-display", ":0", "dpms", "force", "off"],
                       env=env, timeout=5, capture_output=True)
    except Exception:
        pass
    try:
        # Raspberry Pi HDMI aus (funktioniert auch ohne X)
        subprocess.run(["vcgencmd", "display_power", "0"],
                       timeout=5, capture_output=True)
    except Exception:
        pass


def _screen_on_cmd():
    """HDMI und Display-Backlight einschalten."""
    env = {**os.environ, "DISPLAY": ":0", "XAUTHORITY": f"/home/{os.getenv('USER','pi')}/.Xauthority"}
    try:
        subprocess.run(["vcgencmd", "display_power", "1"],
                       timeout=5, capture_output=True)
    except Exception:
        pass
    try:
        subprocess.run(["xset", "-display", ":0", "dpms", "force", "on"],
                       env=env, timeout=5, capture_output=True)
    except Exception:
        pass


def main():
    global _screen_off
    while True:
        try:
            should_save = _is_power_save()
            if should_save and not _screen_off:
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
