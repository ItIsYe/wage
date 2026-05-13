import json
import socket
import sqlite3
import time
from pathlib import Path

DB = Path(__file__).resolve().parents[1] / "data" / "wage_pi.sqlite3"


def set_status(v: str):
    try:
        with sqlite3.connect(DB) as c:
            c.execute("INSERT INTO app_state(key,value) VALUES('oled_status',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (v,))
            c.commit()
    except Exception:
        pass


def get_runtime_status() -> dict:
    with sqlite3.connect(DB) as c:
        c.row_factory = sqlite3.Row
        active = c.execute("SELECT value FROM app_state WHERE key='active_person_id'").fetchone()
        person = c.execute("SELECT id,name FROM persons WHERE id=?", (int(active[0]) if active else 1,)).fetchone()
        dev = c.execute("SELECT last_seen_at FROM devices ORDER BY last_seen_at DESC LIMIT 1").fetchone()
    ip = socket.gethostbyname(socket.gethostname())
    return {"ip": ip, "person": dict(person) if person else {"id": 1, "name": "Unbekannt"}, "last_seen": dev[0] if dev else None}


if __name__ == "__main__":
    set_status("starting")
    try:
        from PIL import Image, ImageDraw
        from luma.core.interface.serial import i2c
        from luma.oled.device import sh1106, ssd1306

        serial = i2c(port=1, address=0x3C)
        try:
            dev = ssd1306(serial)
            driver = "ssd1306"
        except Exception:
            dev = sh1106(serial)
            driver = "sh1106"

        while True:
            s = get_runtime_status()
            img = Image.new("1", dev.size)
            d = ImageDraw.Draw(img)
            d.text((0, 0), f"wage-pi ({driver})", fill=255)
            d.text((0, 14), f"http://{s['ip']}:8000", fill=255)
            d.text((0, 28), f"Person: {s['person']['name'][:12]}", fill=255)
            d.text((0, 42), f"Last: {str(s['last_seen'])[:19]}", fill=255)
            dev.display(img)
            set_status("running")
            time.sleep(5)
    except Exception as exc:
        set_status(f"degraded:{type(exc).__name__}")
        while True:
            time.sleep(30)
