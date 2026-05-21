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
from .config import APP_VERSION, PI_ROOT
from .config_migration import ensure_config_defaults
from .database import get_connection, init_db

app = FastAPI(title="wage-pi", version=APP_VERSION)
app.mount("/static", StaticFiles(directory=str(PI_ROOT / "frontend" / "static")), name="static")
templates = Jinja2Templates(directory=str(PI_ROOT / "frontend" / "templates"))


@app.on_event("startup")
def startup():
    init_db()
    ensure_config_defaults()


@app.get("/api/v1/health")
def health():
    db_ok = True
    try:
        with get_connection() as conn:
            conn.execute("SELECT 1")
    except sqlite3.Error:
        db_ok = False
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


@app.get("/", response_class=HTMLResponse)
def dashboard_page(request: Request):
    return templates.TemplateResponse("dashboard.html", {"request": request})


@app.get("/runs", response_class=HTMLResponse)
def runs_page(request: Request):
    return templates.TemplateResponse("runs.html", {"request": request})


@app.get("/persons", response_class=HTMLResponse)
def persons_page(request: Request):
    return templates.TemplateResponse("persons.html", {"request": request})


@app.get("/status", response_class=HTMLResponse)
def status_page(request: Request):
    return templates.TemplateResponse("status.html", {"request": request})


@app.get("/config", response_class=HTMLResponse)
def config_page(request: Request):
    return templates.TemplateResponse("config.html", {"request": request})
