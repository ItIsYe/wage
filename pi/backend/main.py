from contextlib import asynccontextmanager
from datetime import datetime, timezone
from pathlib import Path
import sqlite3

from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates

from .api_persons import router as persons_router
from .api_runs import router as runs_router
from .api_status import router as status_router
from .api_network_config import router as network_config_router
from .api_update import router as update_router
from .api_esp_firmware import router as esp_firmware_router
from .api_hardware_config import router as hardware_config_router
from .api_stats import router as stats_router
from .api_esp_proxy import router as esp_proxy_router
from .config import APP_VERSION, PI_ROOT
from .config_migration import ensure_config_defaults
from .database import get_connection, init_db


@asynccontextmanager
async def lifespan(app: FastAPI):
    init_db()
    ensure_config_defaults()
    yield


app = FastAPI(title="wage-pi", version=APP_VERSION, lifespan=lifespan)
app.mount("/static", StaticFiles(directory=str(PI_ROOT / "frontend" / "static")), name="static")
templates = Jinja2Templates(directory=str(PI_ROOT / "frontend" / "templates"))


@app.get("/api/v1/health")
def health():
    db_ok = True
    conn = None
    try:
        conn = get_connection()
        conn.execute("SELECT 1")
    except sqlite3.Error:
        db_ok = False
    finally:
        if conn:
            conn.close()
    return {
        "ok": True,
        "service": "wage-pi",
        "version": APP_VERSION,
        "time": datetime.now(timezone.utc).isoformat(),
        "database_ok": db_ok,
    }

app.include_router(runs_router)
app.include_router(persons_router)
app.include_router(status_router)
app.include_router(network_config_router)
app.include_router(update_router)
app.include_router(esp_firmware_router)
app.include_router(hardware_config_router)
app.include_router(stats_router)


@app.get("/stats", response_class=HTMLResponse)
async def page_stats(request: Request):
    return templates.TemplateResponse("stats.html", {"request": request})
app.include_router(esp_proxy_router)


@app.get("/", response_class=HTMLResponse)
def dashboard_page(request: Request):
    return templates.TemplateResponse("dashboard.html", {"request": request})


@app.get("/runs/new", response_class=HTMLResponse)
async def page_run_new(request: Request):
    return templates.TemplateResponse("run_new.html", {"request": request})


@app.get("/runs/{run_id}", response_class=HTMLResponse)
async def page_run_detail(request: Request, run_id: int):
    return templates.TemplateResponse("run_detail.html", {"request": request, "run_id": run_id})


@app.get("/runs", response_class=HTMLResponse)
def runs_page(request: Request):
    return templates.TemplateResponse("runs.html", {"request": request})


@app.get("/persons", response_class=HTMLResponse)
def persons_page(request: Request):
    return templates.TemplateResponse("persons.html", {"request": request})


@app.get("/status", response_class=HTMLResponse)
def status_page(request: Request):
    return templates.TemplateResponse("status.html", {"request": request})


@app.get("/ota", response_class=HTMLResponse)
async def page_ota(request: Request):
    return templates.TemplateResponse("ota.html", {"request": request})


@app.post("/api/v1/system/restart-services")
async def restart_services():
    import subprocess
    try:
        subprocess.Popen(
            ["sudo", "systemctl", "restart", "wage-pi-leds", "wage-pi-oled"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        return {"ok": True}
    except Exception as e:
        return {"ok": False, "detail": str(e)}
async def page_waage_config(request: Request):
    return templates.TemplateResponse("waage_config.html", {"request": request})

@app.get("/config", response_class=HTMLResponse)
def config_page(request: Request):
    return templates.TemplateResponse("config.html", {"request": request})
