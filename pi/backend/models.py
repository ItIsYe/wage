from dataclasses import dataclass


@dataclass(slots=True)
class AppStateEntry:
    key: str
    value: str
