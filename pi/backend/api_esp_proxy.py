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

    return Response(
        content=esp_response.content,
        status_code=esp_response.status_code,
        media_type=esp_response.headers.get("content-type", "text/html"),
    )
