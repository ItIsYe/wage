from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field

from .database import db_cursor

router = APIRouter(prefix="/api/v1/config/hardware", tags=["hardware-config"])


class HardwareConfig(BaseModel):
    pi_led_brightness: int = Field(ge=0, le=255)
    pi_led_strip_count: int = Field(ge=1, le=16)
    pi_led_strip_pixels: int = Field(ge=1, le=500)
    pi_led_strip_sizes: str = Field(default="80,80,82,82")
    pi_oled_rotation: int = Field(ge=0, le=3)
    power_save_after_minutes: int = Field(ge=0, le=1440)


def _read_hardware_config() -> dict:
    with db_cursor() as (_, cur):
        rows = cur.execute(
            "SELECT key, value FROM app_state WHERE key IN "
            "('pi_led_brightness','pi_led_strip_count','pi_led_strip_pixels','pi_led_strip_sizes',"
            "'pi_oled_rotation','power_save_after_minutes')"
        ).fetchall()
    state = {r["key"]: r["value"] for r in rows}
    return {
        "pi_led_brightness": int(state.get("pi_led_brightness", "32")),
        "pi_led_strip_count": int(state.get("pi_led_strip_count", "4")),
        "pi_led_strip_pixels": int(state.get("pi_led_strip_pixels", "40")),
        "pi_led_strip_sizes": state.get("pi_led_strip_sizes", "80,80,82,82"),
        "pi_oled_rotation": int(state.get("pi_oled_rotation", "0")),
        "power_save_after_minutes": int(state.get("power_save_after_minutes", "1")),
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
    return {"ok": True, "message": "Hardware-Konfiguration gespeichert. Dienste neu starten um Änderungen zu übernehmen."}
