import time
from pathlib import Path
import sqlite3

DB = Path(__file__).resolve().parents[1] / "data" / "wage_pi.sqlite3"


def set_status(v: str):
    try:
        with sqlite3.connect(DB) as c:
            c.execute("INSERT INTO app_state(key,value) VALUES('led_status',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (v,))
            c.commit()
    except Exception:
        pass


if __name__ == "__main__":
    set_status("starting")
    while True:
        try:
            # Hier WS2812B-Ansteuerung (z. B. rpi_ws281x) implementieren.
            set_status("running")
        except Exception:
            set_status("error")
        time.sleep(10)
