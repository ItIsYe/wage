from typing import Literal

from fastapi import APIRouter
from pydantic import BaseModel, Field

from .database import db_cursor

router = APIRouter(prefix="/api/v1/config/hardware", tags=["hardware-config"])

_KEYS = ("pi_oled_rotation", "pi_oled_driver", "power_save_after_minutes", "auto_shutdown_after_minutes")


class HardwareConfig(BaseModel):
    # LED-Streifen und -Helligkeit werden ausschließlich über /api/v1/config/leds gepflegt.
    # Früher schrieben beide Seiten dieselben Werte mit unterschiedlichen Vorgaben und überschrieben sich.
    pi_oled_rotation: int = Field(ge=0, le=3)
    # Der Typ laesst sich nicht automatisch erkennen (die SSD1306-Initialisierung klappt auch bei einem SH1106).
    pi_oled_driver: Literal["ssd1306", "sh1106"] = "ssd1306"
    power_save_after_minutes: int = Field(ge=0, le=1440)
    auto_shutdown_after_minutes: int = Field(default=10, ge=0, le=1440)  # 0 = aus


def _int(state: dict, key: str, default: int) -> int:
    try:
        return int(state.get(key, default))
    except (TypeError, ValueError):
        return default


def _read_hardware_config() -> dict:
    with db_cursor() as (_, cur):
        rows = cur.execute(f"SELECT key, value FROM app_state WHERE key IN ({','.join('?' * len(_KEYS))})", _KEYS).fetchall()
    state = {r["key"]: r["value"] for r in rows}
    return {
        "pi_oled_rotation": _int(state, "pi_oled_rotation", 0),
        "pi_oled_driver": state.get("pi_oled_driver") if state.get("pi_oled_driver") in ("ssd1306", "sh1106") else "ssd1306",
        "power_save_after_minutes": _int(state, "power_save_after_minutes", 1),
        "auto_shutdown_after_minutes": _int(state, "auto_shutdown_after_minutes", 10),
    }


@router.get("")
def get_hardware_config():
    return _read_hardware_config()


@router.post("")
def save_hardware_config(cfg: HardwareConfig):
    with db_cursor() as (_, cur):
        for key, value in cfg.model_dump().items():
            cur.execute(
                "INSERT INTO app_state(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                (key, str(value)),
            )
    return {"ok": True, "message": "Hardware-Konfiguration gespeichert. OLED-Drehung und -Typ wirken nach dem Neustart des OLED-Dienstes (erfolgt automatisch)."}
