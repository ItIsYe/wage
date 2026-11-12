import os
from datetime import datetime, timedelta, timezone
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from fastapi import APIRouter

from .database import db_cursor

router = APIRouter(prefix="/api/v1/stats", tags=["stats"])

# Zeiten werden in UTC gespeichert. "Heute" und der Tagesverlauf gelten in der lokalen Zeitzone,
# sonst beginnt der Tag um 01:00 bzw. 02:00 Ortszeit und Läufe nach Mitternacht zählen zum Vortag.
try:
    LOCAL_TZ = ZoneInfo(os.getenv("WAGE_PI_TIMEZONE", "Europe/Berlin"))
except ZoneInfoNotFoundError:  # keine Zeitzonen-Daten installiert
    LOCAL_TZ = timezone.utc

# Zeitpunkt der Messung; ältere Läufe ohne measured_at: Eingangszeit
MEASURED = "COALESCE(measured_at, received_at)"


def _since_iso(period: str) -> str | None:
    """Ab wann gefiltert wird (UTC, ISO); None = gesamt."""
    now = datetime.now(LOCAL_TZ)
    if period == "today":
        start = now.replace(hour=0, minute=0, second=0, microsecond=0)
    elif period == "week":
        start = now - timedelta(days=7)
    elif period == "month":
        start = now - timedelta(days=30)
    else:
        return None
    return start.astimezone(timezone.utc).isoformat()


def _local_day(iso: str) -> str:
    parsed = datetime.fromisoformat(iso)
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(LOCAL_TZ).date().isoformat()


@router.get("")
def get_stats(period: str = "all"):
    since = _since_iso(period)
    where = f"WHERE {MEASURED} >= ?" if since else "WHERE 1=1"
    params = (since,) if since else ()

    with db_cursor() as (_, cur):
        total = cur.execute(f"SELECT COUNT(*) FROM runs {where}", params).fetchone()[0]
        # Läufe mit 0 g / 0 ms (z.B. fehlerhafte Messungen) verfälschen die Mittelwerte nicht
        avg_weight = cur.execute(f"SELECT AVG(start_weight_g) FROM runs {where} AND start_weight_g > 0", params).fetchone()[0]
        avg_time = cur.execute(f"SELECT AVG(time_ms) FROM runs {where} AND time_ms > 0", params).fetchone()[0]
        top_person = cur.execute(
            f"SELECT person_name, COUNT(*) AS cnt FROM runs {where} AND person_name IS NOT NULL AND person_name != '' "
            "GROUP BY person_name ORDER BY cnt DESC LIMIT 1",
            params,
        ).fetchone()
        per_person = cur.execute(
            f"SELECT COALESCE(person_name,'—') AS name, COUNT(*) AS cnt FROM runs {where} "
            "GROUP BY person_name ORDER BY cnt DESC LIMIT 10",
            params,
        ).fetchall()

        # Tagesverlauf: heute und die 6 Tage davor, nach lokalem Datum
        first_day = (datetime.now(LOCAL_TZ) - timedelta(days=6)).replace(hour=0, minute=0, second=0, microsecond=0)
        stamps = cur.execute(
            f"SELECT {MEASURED} FROM runs WHERE {MEASURED} >= ?", (first_day.astimezone(timezone.utc).isoformat(),)
        ).fetchall()

    per_day: dict[str, int] = {}
    for (stamp,) in stamps:
        day = _local_day(stamp)
        per_day[day] = per_day.get(day, 0) + 1

    return {
        "period": period,
        "timezone": getattr(LOCAL_TZ, "key", "UTC"),
        "total_runs": total,
        "avg_weight_g": round(avg_weight, 3) if avg_weight else None,
        "avg_time_ms": round(avg_time) if avg_time else None,
        "top_person": {"name": top_person[0], "count": top_person[1]} if top_person else None,
        "per_person": [{"name": r[0], "count": r[1]} for r in per_person],
        "daily": [{"day": day, "count": per_day[day]} for day in sorted(per_day)],
    }
