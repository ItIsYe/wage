import sqlite3
import time
from pathlib import Path

DB = Path(__file__).resolve().parents[1] / "data" / "wage_pi.sqlite3"


def set_status(v: str):
    try:
        with sqlite3.connect(DB) as c:
            c.execute("INSERT INTO app_state(key,value) VALUES('led_status',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (v,))
            c.commit()
    except Exception:
        pass


if __name__ == "__main__":
    set_status("starting:white")
    try:
        from rpi_ws281x import Color, PixelStrip

        strip = PixelStrip(8, 18)
        strip.begin()

        def fill(c):
            for i in range(strip.numPixels()):
                strip.setPixelColor(i, c)
            strip.show()

        fill(Color(0, 0, 20))
        set_status("running:blue")
        while True:
            set_status("running:blue")
            time.sleep(10)
    except Exception as exc:
        set_status(f"degraded:{type(exc).__name__}")
        while True:
            time.sleep(30)
