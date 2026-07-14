from __future__ import annotations

import httpx
from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import Response

from .database import db_cursor

router = APIRouter(prefix="/esp-proxy", tags=["esp-proxy"])

PROXY_TIMEOUT = 8.0


def _get_esp_ip() -> str:
    with db_cursor() as (_, cur):
        row = cur.execute(
            "SELECT last_ip FROM devices WHERE last_ip IS NOT NULL ORDER BY last_seen_at DESC LIMIT 1"
        ).fetchone()
    if not row or not row["last_ip"]:
        raise HTTPException(status_code=503, detail="ESP-IP unbekannt — noch kein Heartbeat empfangen.")
    return row["last_ip"]


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
    """Proxied alle Anfragen transparent an das ESP32-Webinterface.
    Funktioniert unabhängig vom Netzwerkmodus des Pi."""
    esp_ip = _get_esp_ip()
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
        raise HTTPException(status_code=503, detail=f"ESP nicht erreichbar unter {esp_ip}")
    except httpx.TimeoutException:
        raise HTTPException(status_code=504, detail="Timeout beim Verbinden mit dem ESP")

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
