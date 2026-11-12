from datetime import datetime
from typing import Literal, Optional

from pydantic import BaseModel, Field, field_validator


class RunIn(BaseModel):
    protocol_version: int = Field(default=1)
    device_id: str = Field(min_length=1, max_length=64)
    boot_id: int
    run_number: int = Field(ge=0)
    event_id: str = Field(min_length=1, max_length=128)
    time_ms: int = Field(ge=0)
    start_weight_g: float
    min_weight_g: Optional[float] = None
    start_drop_threshold_g: Optional[float] = None
    stop_rise_threshold_g: Optional[float] = None
    status: str = Field(default="ok", max_length=32)
    firmware_version: str = Field(max_length=64)
    queue_depth: Optional[int] = Field(default=None, ge=0)
    # So lange liegt die Messung beim Absenden zurueck (die Waage puffert Laeufe, wenn der Pi offline ist).
    age_ms: Optional[int] = Field(default=None, ge=0)


class RunBatchIn(BaseModel):
    runs: list[RunIn]


class RunUpdate(BaseModel):
    person_id: Optional[int] = None
    note: Optional[str] = Field(default=None, max_length=200)


class HeartbeatIn(BaseModel):
    device_id: str = Field(min_length=1, max_length=64)
    firmware_version: str = Field(default="", max_length=64)
    boot_id: Optional[int] = None
    queue_depth: Optional[int] = Field(default=None, ge=0)


class ManualRunIn(BaseModel):
    start_weight_g: float = Field(gt=0)
    time_ms: int = Field(default=0, ge=0)
    person_id: Optional[int] = None
    received_at: Optional[datetime] = None
    note: str = Field(default="", max_length=200)
    status: Literal["ok", "manual", "error"] = "manual"


class DebugModeIn(BaseModel):
    enabled: bool


class PersonCreate(BaseModel):
    name: str
    activate: bool = False

    @field_validator("name")
    @classmethod
    def validate_name(cls, value: str) -> str:
        value = value.strip()
        if not value:
            raise ValueError("Name darf nicht leer sein")
        return value


class PersonUpdate(BaseModel):
    name: str
    tags: str = ""
    note: str = ""

    @field_validator("name")
    @classmethod
    def validate_name(cls, value: str) -> str:
        value = value.strip()
        if not value:
            raise ValueError("Name darf nicht leer sein")
        return value


class NetworkConfigIn(BaseModel):
    network_mode: Optional[str] = None
    ap_ssid: Optional[str] = None
    ap_password: Optional[str] = None
    ap_ip: Optional[str] = None
    ap_dhcp_start: Optional[str] = None
    ap_dhcp_end: Optional[str] = None
    client_ssid: Optional[str] = None
    client_password: Optional[str] = None
