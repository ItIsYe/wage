from typing import Optional

from pydantic import BaseModel, Field, field_validator


class RunIn(BaseModel):
    protocol_version: str = Field(default="1.0")
    device_id: str
    boot_id: str
    run_number: int
    event_id: str
    time_ms: int
    start_weight_g: float
    status: str
    firmware_version: str
    queue_depth: Optional[int] = None


class RunBatchIn(BaseModel):
    runs: list[RunIn]


class RunUpdate(BaseModel):
    person_id: Optional[int] = None
    note: Optional[str] = None


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

    @field_validator("name")
    @classmethod
    def validate_name(cls, value: str) -> str:
        value = value.strip()
        if not value:
            raise ValueError("Name darf nicht leer sein")
        return value
