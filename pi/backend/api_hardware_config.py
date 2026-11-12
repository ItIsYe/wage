from fastapi import APIRouter
from pydantic import BaseModel, Field

from .database import db_cursor

router = APIRouter(prefix="/api/v1/config/hardware", tags=["hardware-config"])

_KEYS = ("pi_oled_rotation", "power_save_after_minutes")


class HardwareConfig(BaseModel):
    # LED-Streifen und -Helligkeit werden ausschließlich über /api/v1/config/leds gepflegt.
    # Früher schrieben beide Seiten dieselben Werte mit unterschiedlichen Vorgaben und überschrieben sich.
    pi_oled_rotation: int = Field(ge=0, le=3)
    power_save_after_minutes: int = Field(ge=0, le=1440)


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
        "power_save_after_minutes": _int(state, "power_save_after_minutes", 1),
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
    return {"ok": True, "message": "Hardware-Konfiguration gespeichert. Die OLED-Drehung wirkt nach dem Neustart des OLED-Dienstes."}
