from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field

from .database import db_cursor

router = APIRouter(prefix="/api/v1/config/hardware", tags=["hardware-config"])


class HardwareConfig(BaseModel):
    pi_led_count: int = Field(ge=1, le=1000)
    pi_led_brightness: int = Field(ge=0, le=255)
    pi_oled_rotation: int = Field(ge=0, le=3)


def _read_hardware_config() -> dict:
    with db_cursor() as (_, cur):
        rows = cur.execute(
            "SELECT key, value FROM app_state WHERE key IN ('pi_led_count','pi_led_brightness','pi_oled_rotation')"
        ).fetchall()
    state = {r["key"]: r["value"] for r in rows}
    return {
        "pi_led_count": int(state.get("pi_led_count", "8")),
        "pi_led_brightness": int(state.get("pi_led_brightness", "32")),
        "pi_oled_rotation": int(state.get("pi_oled_rotation", "0")),
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
