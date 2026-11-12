import json
import sqlite3
import subprocess
import sys
from datetime import datetime, timedelta, timezone
from hashlib import sha256
from pathlib import Path

import pytest

PI_DIR = Path(__file__).resolve().parents[1]


def run_payload(n=1, **extra):
    p = {"protocol_version": 1, "device_id": "waage-01", "boot_id": 7, "run_number": n,
         "event_id": f"waage-01-7-{n}", "time_ms": 4200, "start_weight_g": 335.0,
         "status": "ok", "firmware_version": "beta-test", "queue_depth": 0}
    p.update(extra)
    return p


def db():
    from backend.config import DB_PATH
    c = sqlite3.connect(DB_PATH)
    c.row_factory = sqlite3.Row
    return c


def test_health(client):
    r = client.get("/api/v1/health")
    assert r.status_code == 200 and r.json()["database_ok"] is True


def test_run_and_duplicate(client):
    r1 = client.post("/api/v1/runs", json=run_payload())
    assert r1.status_code == 201 and r1.json()["duplicate"] is False
    r2 = client.post("/api/v1/runs", json=run_payload())
    assert r2.json()["duplicate"] is True and r2.json()["run_db_id"] == r1.json()["run_db_id"]


def test_invalid_run_is_rejected(client):
    assert client.post("/api/v1/runs", json=run_payload(time_ms=-1)).status_code == 422
    assert client.post("/api/v1/runs/heartbeat", json={"device_id": ""}).status_code == 422


def test_run_gets_person_active_at_measurement_time(client):
    anna = client.post("/api/v1/persons", json={"name": "Anna", "activate": True}).json()["person_id"]
    # Lauf wurde vor 1 h gemessen, als noch "Unbekannt" aktiv war -> nicht Anna
    with db() as c:
        c.execute("UPDATE person_activations SET activated_at=? WHERE person_id=?",
                  ((datetime.now(timezone.utc) - timedelta(minutes=30)).isoformat(), anna))
    r = client.post("/api/v1/runs", json=run_payload(age_ms=3_600_000)).json()
    assert r["active_person_id"] == 1
    r2 = client.post("/api/v1/runs", json=run_payload(2, age_ms=1000)).json()
    assert r2["active_person_id"] == anna


def test_deleted_person_history_goes_to_unknown(client):
    a = client.post("/api/v1/persons", json={"name": "A", "activate": True}).json()["person_id"]
    t_a = datetime.now(timezone.utc)
    b = client.post("/api/v1/persons", json={"name": "B", "activate": True}).json()["person_id"]
    with db() as c:  # A ab vor 20 min, B ab vor 10 min
        c.execute("UPDATE person_activations SET activated_at=? WHERE person_id=?", ((t_a - timedelta(minutes=20)).isoformat(), a))
        c.execute("UPDATE person_activations SET activated_at=? WHERE person_id=?", ((t_a - timedelta(minutes=10)).isoformat(), b))
    assert client.delete(f"/api/v1/persons/{b}").status_code == 200
    # Lauf aus Bs Zeit kommt verspaetet an: gehoert jetzt "Unbekannt", nicht A
    r = client.post("/api/v1/runs", json=run_payload(5, age_ms=5 * 60 * 1000)).json()
    assert r["active_person_id"] == 1


def test_heartbeat_and_status(client):
    client.post("/api/v1/runs", json=run_payload(age_ms=1000))
    client.post("/api/v1/runs/heartbeat", json={"device_id": "waage-01", "boot_id": 4294967295, "firmware_version": "x"})
    from backend import api_status
    api_status._status_cache = {}
    s = client.get("/api/v1/status").json()
    assert s["scale_online"] is True
    assert s["last_device"]["last_boot_id"] == 4294967295
    assert s["recent_runs"][0]["measured_at"]  # Dashboard zeigt die Messzeit
    assert s["api_status"] == "ok" and s["database_status"] == "ok"


def test_runs_list_and_unassign_person(client):
    rid = client.post("/api/v1/runs", json=run_payload(note="x")).json()["run_db_id"]
    assert client.put(f"/api/v1/runs/{rid}", json={"person_id": None}).status_code == 200
    assert client.get(f"/api/v1/runs/{rid}").json()["run"]["person_id"] is None
    # % in der Suche ist ein normales Zeichen
    assert client.get("/api/v1/runs", params={"search": "%"}).json()["count"] == 0


def test_csv_export(client):
    client.post("/api/v1/persons", json={"name": "=1+1", "activate": True})
    client.post("/api/v1/runs", json=run_payload())
    text = client.get("/api/v1/runs/export/csv").text
    assert text.startswith("﻿") and "'=1+1" in text and "measured_at" in text


def test_stats(client):
    client.post("/api/v1/runs", json=run_payload())
    client.post("/api/v1/runs", json=run_payload(2, start_weight_g=0, time_ms=0))
    s = client.get("/api/v1/stats", params={"period": "today"}).json()
    assert s["total_runs"] == 2 and s["avg_weight_g"] == 335.0 and s["daily"][-1]["count"] == 2


def test_hardware_config_roundtrip(client):
    cfg = {"pi_oled_rotation": 2, "pi_oled_driver": "sh1106", "power_save_after_minutes": 5, "auto_shutdown_after_minutes": 0}
    assert client.post("/api/v1/config/hardware", json=cfg).status_code == 200
    assert client.get("/api/v1/config/hardware").json() == cfg
    assert client.post("/api/v1/config/hardware", json={**cfg, "pi_oled_driver": "xyz"}).status_code == 422


def test_led_config_validation(client):
    base = {"pi_led_strip_sizes": "80,80", "led_brightness_rainbow": 1, "led_brightness_pulse_offline": 1,
            "led_brightness_wave": 1, "led_brightness_power_save": 1, "led_brightness_boot": 1, "led_brightness_error": 1}
    assert client.post("/api/v1/config/leds", json=base).status_code == 200
    assert client.post("/api/v1/config/leds", json={**base, "pi_led_strip_sizes": "0,5"}).status_code == 422
    assert client.get("/api/v1/config/leds").json()["pi_led_strip_sizes"] == "80,80"


def test_network_config_validation(client):
    ok = {"network_mode": "ap", "ap_ssid": "wage-net", "ap_password": "geheim123", "ap_ip": "192.168.50.1",
          "ap_dhcp_start": "192.168.50.50", "ap_dhcp_end": "192.168.50.150"}
    r = client.post("/api/v1/config/network", json=ok)
    assert r.status_code == 200 and "client_dhcp_enabled" not in r.json()["config"]
    assert client.post("/api/v1/config/network", json={**ok, "ap_dhcp_start": "192.168.50.1"}).status_code == 400
    assert client.post("/api/v1/config/network", json={**ok, "ap_dhcp_end": "10.0.0.5"}).status_code == 400


def _fake_release(fw, monkeypatch, manifest_sha, fw_bytes, version):
    calls = {"firmware": 0}
    assets = [{"name": "manifest.json", "_data": json.dumps({"version": version, "sha256": manifest_sha}).encode()},
              {"name": "firmware.bin", "_data": fw_bytes}]
    monkeypatch.setattr(fw, "_fetch_release", lambda: {"assets": assets})

    def dl(asset, dest):
        if asset["name"] == "firmware.bin":
            calls["firmware"] += 1
        Path(dest).write_bytes(asset["_data"])
    monkeypatch.setattr(fw, "_download_asset", dl)
    return calls


def test_firmware_cache(client, monkeypatch, tmp_path):
    from backend import api_esp_firmware as fw
    for name, value in {"CACHE_DIR": tmp_path, "MANIFEST_CACHE_PATH": tmp_path / "manifest.json",
                        "FIRMWARE_CACHE_PATH": tmp_path / "firmware.bin", "FIRMWARE_META_PATH": tmp_path / "firmware_meta.json",
                        "LOCK_PATH": tmp_path / "sync.lock"}.items():
        monkeypatch.setattr(fw, name, value)
    good = b"firmware-v1" * 100
    _fake_release(fw, monkeypatch, "0" * 64, good, "v1")
    assert client.post("/api/v1/esp-firmware/sync").status_code == 502       # falsche Pruefsumme
    assert not fw.FIRMWARE_CACHE_PATH.exists()
    calls = _fake_release(fw, monkeypatch, sha256(good).hexdigest(), good, "v1")
    assert client.post("/api/v1/esp-firmware/sync").status_code == 200
    assert client.get("/api/v1/esp-firmware/latest.bin").content == good
    # Zweiter Sync derselben Firmware laedt sie nicht erneut herunter
    r = client.post("/api/v1/esp-firmware/sync").json()
    assert r.get("cached") is True and calls["firmware"] == 1
    # "Pruefen" mit neuerem Release darf das zur Firmware gehoerende Manifest nicht ersetzen
    _fake_release(fw, monkeypatch, sha256(b"v2").hexdigest(), b"v2", "v2")
    assert client.get("/api/v1/esp-firmware/manifest", params={"refresh": "true"}).json()["version"] == "v2"
    assert client.get("/api/v1/esp-firmware/manifest").json()["version"] == "v1"


def test_update_state_cli(client):
    from backend.api_update import _read_state
    out = subprocess.run([sys.executable, "-m", "backend.update_state", "updating", "s", "m", "Schritt", "40"], cwd=PI_DIR)
    assert out.returncode == 0
    assert _read_state(["last_update_progress_percent"])["last_update_progress_percent"] == "40"
    assert subprocess.run([sys.executable, "-m", "backend.update_state", "zu", "wenig"], cwd=PI_DIR,
                          capture_output=True).returncode == 2


def test_seconds_since_latest():
    from common.runtime import seconds_since_latest
    now = datetime.now(timezone.utc)
    age = seconds_since_latest((now - timedelta(minutes=30)).isoformat(), (now - timedelta(minutes=2)).isoformat(), None, "kaputt")
    assert 100 < age < 140
    assert seconds_since_latest(None, "") is None


@pytest.mark.parametrize("path", ["/", "/runs", "/persons", "/status", "/config", "/config/leds", "/stats", "/ota", "/runs/new"])
def test_pages_render(client, path):
    r = client.get(path)
    assert r.status_code == 200 and "app.js" in r.text and "keyboard.js" in r.text and "fonts.googleapis" not in r.text


def test_esp_proxy_injects_keyboard(client, monkeypatch):
    import httpx
    from backend import api_esp_proxy

    class FakeClient:
        def __init__(self, *a, **k): pass
        async def __aenter__(self): return self
        async def __aexit__(self, *a): return False
        async def request(self, **k):
            return httpx.Response(200, headers={"content-type": "text/html"},
                                  content=b"<html><body><form action='/save'><input name='deviceId'></form></body></html>")
    monkeypatch.setattr(api_esp_proxy.httpx, "AsyncClient", FakeClient)
    monkeypatch.setattr(api_esp_proxy, "_get_esp_ip", lambda: "127.0.0.1")
    html = client.get("/esp-proxy/").text
    assert "/static/keyboard.js" in html and "/static/keyboard.css" in html and "action='/esp-proxy/save'" in html
    assert client.get("/static/keyboard.js").status_code == 200
