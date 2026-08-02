from datetime import datetime, timezone, timedelta
from fastapi import APIRouter

from .database import db_cursor

router = APIRouter(prefix="/api/v1/stats", tags=["stats"])


def _date_filter(period: str) -> str | None:
    """Gibt ISO-Datum zurück ab dem gefiltert wird."""
    now = datetime.now(timezone.utc)
    if period == "today":
        return now.replace(hour=0, minute=0, second=0, microsecond=0).isoformat()
    elif period == "week":
        return (now - timedelta(days=7)).isoformat()
    elif period == "month":
        return (now - timedelta(days=30)).isoformat()
    return None  # gesamt


@router.get("")
def get_stats(period: str = "all"):
    since = _date_filter(period)

    with db_cursor() as (_, cur):
        where = "WHERE received_at >= ?" if since else ""
        params = (since,) if since else ()

        # Gesamt-Läufe
        total = cur.execute(f"SELECT COUNT(*) FROM runs {where}", params).fetchone()[0]

        # Durchschnittsgewicht
        avg_weight = cur.execute(
            f"SELECT AVG(start_weight_g) FROM runs {where}", params
        ).fetchone()[0]

        # Durchschnittszeit
        avg_time = cur.execute(
            f"SELECT AVG(time_ms) FROM runs {where} AND time_ms > 0"
            if since else "SELECT AVG(time_ms) FROM runs WHERE time_ms > 0",
            params if since else ()
        ).fetchone()[0]

        # Häufigste Person
        top_person = cur.execute(
            f"SELECT person_name, COUNT(*) as cnt FROM runs {where} "
            f"AND person_name IS NOT NULL AND person_name != '' "
            f"GROUP BY person_name ORDER BY cnt DESC LIMIT 1"
            if since else
            "SELECT person_name, COUNT(*) as cnt FROM runs "
            "WHERE person_name IS NOT NULL AND person_name != '' "
            "GROUP BY person_name ORDER BY cnt DESC LIMIT 1",
            params if since else ()
        ).fetchone()

        # Läufe pro Person
        per_person = cur.execute(
            f"SELECT COALESCE(person_name,'—') as name, COUNT(*) as cnt FROM runs {where} "
            f"GROUP BY person_name ORDER BY cnt DESC LIMIT 10",
            params
        ).fetchall()

        # Tagesverlauf (letzte 7 Tage)
        daily = cur.execute(
            "SELECT DATE(received_at) as day, COUNT(*) as cnt "
            "FROM runs WHERE received_at >= ? "
            "GROUP BY day ORDER BY day",
            ((datetime.now(timezone.utc) - timedelta(days=7)).isoformat(),)
        ).fetchall()

    return {
        "period": period,
        "total_runs": total,
        "avg_weight_g": round(avg_weight, 3) if avg_weight else None,
        "avg_time_ms": round(avg_time) if avg_time else None,
        "top_person": {"name": top_person[0], "count": top_person[1]} if top_person else None,
        "per_person": [{"name": r[0], "count": r[1]} for r in per_person],
        "daily": [{"day": r[0], "count": r[1]} for r in daily],
    }
