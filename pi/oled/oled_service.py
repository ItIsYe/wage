import os
import sqlite3
import time
from pathlib import Path

# Der Dienst wird als "python <ordner>/<dienst>.py" gestartet; sys.path[0] ist dann der Unterordner,
# nicht pi/. Ohne das Elternverzeichnis liessen sich backend/ und common/ nicht importieren.
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common.runtime import get_pi_ip, seconds_since  # noqa: E402

DB = Path(__file__).resolve().parents[1] / "data" / "wage_pi.sqlite3"
OLED_ADDRESS = int(os.getenv("WAGE_PI_OLED_ADDRESS", "0x3c"), 16)
REINIT_SECONDS = 10


def _get_hw_config() -> dict:
    try:
        with sqlite3.connect(DB) as c:
            rows = c.execute(
                "SELECT key, value FROM app_state WHERE key IN ('pi_oled_rotation','power_save_after_minutes','last_run_received_at')"
            ).fetchall()
            return {r[0]: r[1] for r in rows}
    except Exception:
        return {}


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
        age = seconds_since(last_run)
        if age is None:
            return False
        return age / 60 >= minutes
    except Exception:
        return False

try:
    from backend.config import OFFLINE_THRESHOLD_SECONDS
except Exception:
    OFFLINE_THRESHOLD_SECONDS = 90


def set_status(v: str):
    try:
        with sqlite3.connect(DB) as c:
            c.execute(
                "INSERT INTO app_state(key,value) VALUES('oled_status',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                (v,),
            )
            c.commit()
    except Exception:
        pass


def get_runtime_status() -> dict:
    with sqlite3.connect(DB) as c:
        c.row_factory = sqlite3.Row
        app = {r["key"]: r["value"] for r in c.execute("SELECT key,value FROM app_state")}
        active = int(app.get("active_person_id", "1"))
        person = c.execute("SELECT id,name FROM persons WHERE id=?", (active,)).fetchone()
        dev = c.execute("SELECT last_seen_at FROM devices ORDER BY last_seen_at DESC LIMIT 1").fetchone()
    ip = get_pi_ip()
    last_seen = dev["last_seen_at"] if dev else None
    age = seconds_since(last_seen)
    online = age is not None and age <= OFFLINE_THRESHOLD_SECONDS
    return {
        "url": f"http://{ip}:8000",
        "ip": ip,
        "person": dict(person) if person else {"id": 1, "name": "Unbekannt"},
        "last_seen": last_seen,
        "scale_online": online,
        "api_status": app.get("api_status", "ok"),
        "database_status": app.get("database_status", "ok"),
    }


if __name__ == "__main__":
    set_status("starting")
    dev = None
    Image = ImageDraw = qrcode = None
    driver = "unknown"
    qr_cache_url = None
    qr_cache_img = None
    last_try = 0.0

    while True:
        now = time.time()
        if dev is None and now - last_try >= REINIT_SECONDS:
            last_try = now
            try:
                from PIL import Image, ImageDraw
                from luma.core.interface.serial import i2c
                from luma.oled.device import sh1106, ssd1306
                try:
                    import qrcode
                except Exception:
                    qrcode = None

                serial = i2c(port=1, address=OLED_ADDRESS)
                hw = _get_hw_config()
                rotation = int(hw.get("pi_oled_rotation", "0"))
                try:
                    dev = ssd1306(serial, rotate=rotation)
                    driver = "ssd1306"
                except Exception:
                    dev = sh1106(serial, rotate=rotation)
                    driver = "sh1106"
                set_status(f"running:{driver}")
            except Exception as exc:
                set_status(f"degraded:{type(exc).__name__}")
                dev = None

        try:
            if dev is None:
                time.sleep(2)
                continue

            # Energiesparmodus: Display aus
            if _is_power_save():
                dev.hide()
                set_status("power_save")
                time.sleep(10)  # Im Power-Save seltener prüfen
                continue
            else:
                dev.show()

            s = get_runtime_status()
            img = Image.new("1", dev.size)
            draw = ImageDraw.Draw(img)

            can_qr = qrcode is not None and min(dev.size) >= 64
            if can_qr and (qr_cache_img is None or qr_cache_url != s["url"]):
                qr = qrcode.QRCode(border=1, box_size=2)
                qr.add_data(s["url"])
                qr.make(fit=True)
                qr_cache_img = qr.make_image(fill_color="black", back_color="white").convert("1")
                qr_cache_url = s["url"]

            show_qr = can_qr and int(now) % 10 < 4
            if show_qr and qr_cache_img is not None:
                img.paste(qr_cache_img.resize((64, 64)), (0, 0))
                draw.text((68, 0), "wage-pi", fill=255)
                draw.text((68, 14), s["ip"][:15], fill=255)
                draw.text((68, 28), "Web :8000", fill=255)
                draw.text((68, 42), driver, fill=255)
            else:
                draw.text((0, 0), "wage-pi", fill=255)
                draw.text((0, 12), s["url"][:22], fill=255)
                draw.text((0, 24), f"Aktiv: {s['person']['name'][:12]}", fill=255)
                draw.text((0, 36), f"Waage: {'online' if s['scale_online'] else 'offline'}", fill=255)
                draw.text((0, 48), f"API/DB: {s['api_status']}/{s['database_status']}", fill=255)

            dev.display(img)
            set_status(f"running:{driver}")
            time.sleep(2)
        except Exception as exc:
            set_status(f"degraded:{type(exc).__name__}")
            dev = None
            time.sleep(2)
