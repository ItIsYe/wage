"""Gemeinsame Hilfsfunktionen fuer Backend und Hintergrunddienste (LED, OLED, Screen-Save).

Vorher gab es IP-Ermittlung und Zeitstempel-Parsing mehrfach, mit leicht unterschiedlichem Verhalten.
Besonders Zeitstempel ohne Zeitzone (z.B. aus sqlite datetime('now')) fuehrten zu einem TypeError beim
Vergleich mit aware-Zeiten, den die Dienste still als "kein Power-Save" werteten.
"""
from __future__ import annotations

import subprocess
import time
from datetime import datetime, timezone

_IP_CACHE_TTL = 30.0  # Sekunden
_ip_cache: str = ""
_ip_cache_at: float = 0.0


def parse_ts(value: str | None) -> datetime | None:
    """ISO-Zeitstempel -> aware datetime (UTC). Werte ohne Zeitzone gelten als UTC. Ungueltig -> None."""
    if not value:
        return None
    text = str(value).strip()
    if text.endswith(("Z", "z")):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError:
        return None
    return parsed if parsed.tzinfo else parsed.replace(tzinfo=timezone.utc)


def seconds_since(value: str | None) -> float | None:
    """Sekunden seit dem Zeitstempel; None, wenn er fehlt oder ungueltig ist."""
    parsed = parse_ts(value)
    if parsed is None:
        return None
    return (datetime.now(timezone.utc) - parsed).total_seconds()


def get_pi_ip() -> str:
    """Erste nicht-lokale IPv4-Adresse des Pi (nmcli, sonst hostname -I). 30 s gecacht."""
    global _ip_cache, _ip_cache_at
    if _ip_cache and (time.monotonic() - _ip_cache_at) < _IP_CACHE_TTL:
        return _ip_cache
    candidates: list[str] = []
    try:
        out = subprocess.check_output(["nmcli", "-t", "-f", "IP4.ADDRESS", "device", "show"], text=True, timeout=3)
        for line in out.splitlines():
            if line.startswith("IP4.ADDRESS"):
                addr = line.split(":")[-1].split("/")[0].strip()
                if addr:
                    candidates.append(addr)
    except (OSError, subprocess.SubprocessError):
        pass
    if not candidates:
        try:
            out = subprocess.check_output(["hostname", "-I"], text=True, timeout=3)
            candidates = out.split()
        except (OSError, subprocess.SubprocessError):
            pass
    for addr in candidates:
        if not addr.startswith("127."):
            _ip_cache, _ip_cache_at = addr, time.monotonic()
            return addr
    return "127.0.0.1"
