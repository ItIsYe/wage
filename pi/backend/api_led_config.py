from fastapi import APIRouter
from pydantic import BaseModel, Field

from .database import db_cursor

router = APIRouter(prefix="/api/v1/config/leds", tags=["led-config"])

LED_KEYS = [
    "pi_led_strip_sizes",
    "pi_led_brightness",
    "led_brightness_rainbow",
    "led_brightness_pulse_offline",
    "led_brightness_wave",
    "led_brightness_power_save",
    "led_brightness_boot",
    "led_brightness_error",
]


class LedConfig(BaseModel):
    pi_led_strip_sizes: str = Field(default="80,80,82,82")
    pi_led_brightness: int = Field(ge=0, le=255)
    led_brightness_rainbow: int = Field(ge=0, le=255)
    led_brightness_pulse_offline: int = Field(ge=0, le=255)
    led_brightness_wave: int = Field(ge=0, le=255)
    led_brightness_power_save: int = Field(ge=0, le=255)
    led_brightness_boot: int = Field(ge=0, le=255)
    led_brightness_error: int = Field(ge=0, le=255)


@router.get("")
def get_led_config():
    with db_cursor() as (_, cur):
        rows = cur.execute(
            f"SELECT key, value FROM app_state WHERE key IN ({','.join('?'*len(LED_KEYS))})",
            LED_KEYS
        ).fetchall()
    state = {r["key"]: r["value"] for r in rows}
    return {
        "pi_led_strip_sizes": state.get("pi_led_strip_sizes", "80,80,82,82"),
        "pi_led_brightness": int(state.get("pi_led_brightness", "32")),
        "led_brightness_rainbow": int(state.get("led_brightness_rainbow", "80")),
        "led_brightness_pulse_offline": int(state.get("led_brightness_pulse_offline", "60")),
        "led_brightness_wave": int(state.get("led_brightness_wave", "120")),
        "led_brightness_power_save": int(state.get("led_brightness_power_save", "12")),
        "led_brightness_boot": int(state.get("led_brightness_boot", "40")),
        "led_brightness_error": int(state.get("led_brightness_error", "100")),
    }


@router.post("")
def save_led_config(cfg: LedConfig):
    with db_cursor() as (_, cur):
        for key, value in cfg.model_dump().items():
            cur.execute(
                "INSERT INTO app_state(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                (key, str(value))
            )
    return {"ok": True, "message": "LED-Konfiguration gespeichert. Wird sofort übernommen."}
