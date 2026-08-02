from __future__ import annotations

import httpx
from fastapi import APIRouter, Request
from fastapi.responses import HTMLResponse, Response

from .database import db_cursor

router = APIRouter(prefix="/esp-proxy", tags=["esp-proxy"])

PROXY_TIMEOUT = 5.0

_ERROR_PAGE = """<!doctype html><html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Waage nicht erreichbar</title>
<style>
body{{font-family:Arial,sans-serif;display:flex;flex-direction:column;align-items:center;
justify-content:center;min-height:60vh;margin:0;background:#f4f6f8;color:#1f2937}}
.card{{background:#fff;border-radius:12px;padding:2em 2.5em;box-shadow:0 2px 8px rgba(0,0,0,.08);
text-align:center;max-width:400px}}
h2{{color:#dc2626;margin-top:0}}
p{{color:#6b7280}}
a{{display:inline-block;margin-top:1em;padding:.6em 1.4em;background:#2563eb;color:#fff;
text-decoration:none;border-radius:8px;font-weight:700}}
</style></head><body>
<div class="card">
<h2>⚠️ Waage nicht erreichbar</h2>
<p>{detail}</p>
<p>Bitte sicherstellen dass die Waage eingeschaltet und mit <strong>wage-net</strong> verbunden ist.</p>
<a href="/runs">← Zurück zu den Läufen</a>
</div></body></html>"""


def _get_esp_ip() -> str | None:
    try:
        with db_cursor() as (_, cur):
            row = cur.execute(
                "SELECT last_ip FROM devices WHERE last_ip IS NOT NULL ORDER BY last_seen_at DESC LIMIT 1"
            ).fetchone()
        return row["last_ip"] if row and row["last_ip"] else None
    except Exception:
        return None


def _error_page(detail: str) -> HTMLResponse:
    return HTMLResponse(content=_ERROR_PAGE.format(detail=detail), status_code=200)


@router.get("/", operation_id="esp_proxy_get_root")
async def proxy_to_esp_get_root(request: Request):
    return await _proxy("", request)


@router.get("/{path:path}", operation_id="esp_proxy_get")
async def proxy_to_esp_get(path: str, request: Request):
    return await _proxy(path, request)


@router.post("/{path:path}", operation_id="esp_proxy_post")
async def proxy_to_esp_post(path: str, request: Request):
    return await _proxy(path, request)


async def _proxy(path: str, request: Request):
    esp_ip = _get_esp_ip()
    if not esp_ip:
        return _error_page("ESP-IP unbekannt — noch kein Heartbeat empfangen.")

    url = f"http://{esp_ip}/{path}"
    if request.url.query:
        url += f"?{request.url.query}"

    try:
        async with httpx.AsyncClient(timeout=PROXY_TIMEOUT) as client:
            esp_response = await client.request(
                method=request.method,
                url=url,
                headers={
                    k: v for k, v in request.headers.items()
                    if k.lower() not in ("host", "content-length")
                },
                content=await request.body(),
                follow_redirects=True,
            )
    except httpx.ConnectError:
        return _error_page(f"Verbindung zu {esp_ip} fehlgeschlagen.")
    except httpx.TimeoutException:
        return _error_page(f"Timeout beim Verbinden mit {esp_ip}.")
    except Exception as e:
        return _error_page(f"Unbekannter Fehler: {e}")

    content_type = esp_response.headers.get("content-type", "text/html; charset=utf-8")
    content = esp_response.content
    if "text/html" not in content_type and content.strip().startswith(b"<"):
        content_type = "text/html; charset=utf-8"

    # HTML-Inhalte anpassen
    if "text/html" in content_type:
        # Relative Links und Actions über den Proxy leiten
        content = content.replace(b"action='/", b"action='/esp-proxy/")
        content = content.replace(b'action="/', b'action="/esp-proxy/')
        content = content.replace(b"href='/", b"href='/esp-proxy/")
        content = content.replace(b'href="/', b'href="/esp-proxy/')
        # /ota Link auf Pi-OTA-Seite umleiten
        content = content.replace(b"href='/esp-proxy/ota'", b"href='/ota'")
        content = content.replace(b'href="/esp-proxy/ota"', b'href="/ota"')
        # Pi-Navigation einfügen
        nav = (
            b"<header><nav style='background:#111827;padding:16px;position:sticky;top:0;z-index:999;'>"
            b"<a href='/' style='color:#fff;text-decoration:none;font-weight:700;padding:10px 14px;border-radius:10px;margin-right:4px;'>Dashboard</a>"
            b"<a href='/runs' style='color:#fff;text-decoration:none;font-weight:700;padding:10px 14px;border-radius:10px;margin-right:4px;'>L\xc3\xa4ufe</a>"
            b"<a href='/persons' style='color:#fff;text-decoration:none;font-weight:700;padding:10px 14px;border-radius:10px;margin-right:4px;'>Personen</a>"
            b"<a href='/status' style='color:#fff;text-decoration:none;font-weight:700;padding:10px 14px;border-radius:10px;margin-right:4px;'>Status</a>"
            b"<a href='/config' style='color:#fff;text-decoration:none;font-weight:700;padding:10px 14px;border-radius:10px;margin-right:4px;'>Konfiguration</a>"
            b"<a href='/esp-proxy/' style='color:#fff;text-decoration:none;font-weight:700;padding:10px 14px;border-radius:10px;margin-right:4px;background:#2563eb;'>Waage-Config</a>"
            b"<a href='/ota' style='color:#fff;text-decoration:none;font-weight:700;padding:10px 14px;border-radius:10px;'>OTA-Update</a>"
            b"</nav></header>"
        )
        content = content.replace(b"<body>", b"<body>" + nav)
        content = content.replace(b"<body ", b"<body>" + nav + b"<div ")

    return Response(
        content=content,
        status_code=esp_response.status_code,
        media_type=content_type,
    )
