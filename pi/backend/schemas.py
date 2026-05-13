from datetime import datetime
from typing import Optional

from pydantic import BaseModel, Field


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


class PersonCreate(BaseModel):
    name: str
    activate: bool = False


class PersonUpdate(BaseModel):
    name: str
