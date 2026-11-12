import re
from typing import Optional

from fastapi import APIRouter
from pydantic import BaseModel, Field, field_validator

from .database import db_cursor

router = APIRouter(prefix="/api/v1/config/leds", tags=["led-config"])

DEFAULT_STRIP_SIZES = "80,80,82,82"
MAX_TOTAL_PIXELS = 2000
_SIZES_RE = re.compile(r"^\d{1,4}(,\d{1,4})*$")

# Vorgaben wie in config_defaults.py (Regenbogen/Welle/Boot sind die Strip-Helligkeit des jeweiligen Musters)
LED_DEFAULTS = {
    "pi_led_strip_sizes": DEFAULT_STRIP_SIZES,
    "pi_led_brightness": "32",
    "led_brightness_rainbow": "80",
    "led_brightness_pulse_offline": "60",
    "led_brightness_wave": "120",
    "led_brightness_power_save": "12",
    "led_brightness_boot": "40",
    "led_brightness_error": "100",
}
LED_KEYS = list(LED_DEFAULTS)


class LedConfig(BaseModel):
    pi_led_strip_sizes: str = Field(default=DEFAULT_STRIP_SIZES)
    # Optional: fehlt der Wert, bleibt der gespeicherte Stand unverändert.
    pi_led_brightness: Optional[int] = Field(default=None, ge=0, le=255)
    led_brightness_rainbow: int = Field(ge=0, le=255)
    led_brightness_pulse_offline: int = Field(ge=0, le=255)
    led_brightness_wave: int = Field(ge=0, le=255)
    led_brightness_power_save: int = Field(ge=0, le=255)
    led_brightness_boot: int = Field(ge=0, le=255)
    led_brightness_error: int = Field(ge=0, le=255)

    @field_validator("pi_led_strip_sizes")
    @classmethod
    def validate_sizes(cls, value: str) -> str:
        value = value.replace(" ", "")
        if not _SIZES_RE.match(value):
            raise ValueError("Streifengrößen als Zahlen mit Komma getrennt angeben, z.B. 80,80,82,82")
        sizes = [int(x) for x in value.split(",")]
        if any(x < 1 for x in sizes) or sum(sizes) > MAX_TOTAL_PIXELS:
            raise ValueError(f"Jeder Streifen braucht mindestens 1 Pixel, insgesamt höchstens {MAX_TOTAL_PIXELS}")
        return value


def _as_int(text, default: str) -> int:
    try:
        return int(text)
    except (TypeError, ValueError):
        return int(default)


@router.get("")
def get_led_config():
    with db_cursor() as (_, cur):
        rows = cur.execute(
            f"SELECT key, value FROM app_state WHERE key IN ({','.join('?' * len(LED_KEYS))})", LED_KEYS
        ).fetchall()
    state = {r["key"]: r["value"] for r in rows}
    result = {"pi_led_strip_sizes": state.get("pi_led_strip_sizes") or DEFAULT_STRIP_SIZES}
    for key, default in LED_DEFAULTS.items():
        if key != "pi_led_strip_sizes":
            result[key] = _as_int(state.get(key, default), default)
    return result


@router.post("")
def save_led_config(cfg: LedConfig):
    with db_cursor() as (_, cur):
        for key, value in cfg.model_dump(exclude_none=True).items():
            cur.execute(
                "INSERT INTO app_state(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                (key, str(value)),
            )
    return {"ok": True, "message": "LED-Konfiguration gespeichert. Wird innerhalb weniger Sekunden übernommen."}
