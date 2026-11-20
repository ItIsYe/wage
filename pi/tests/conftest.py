"""Gemeinsame Test-Umgebung: eigener Datenordner pro Testlauf, pi/ im Importpfad."""
import os
import sys
import tempfile
from pathlib import Path

import pytest

PI_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PI_DIR))
# Vor dem ersten Import von backend setzen: config.py liest den Datenordner beim Import
os.environ["WAGE_PI_DATA_DIR"] = tempfile.mkdtemp(prefix="wage-pi-test-")


@pytest.fixture()
def client():
    from fastapi.testclient import TestClient
    from backend.config import DB_PATH
    from backend.main import app

    for suffix in ("", "-wal", "-shm"):
        Path(str(DB_PATH) + suffix).unlink(missing_ok=True)
    with TestClient(app) as c:
        yield c
