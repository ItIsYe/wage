# WAGE Raspberry Pi

## Projektziel
Der Raspberry Pi ist die lokale Datenzentrale für die Waage: FastAPI-Backend, SQLite-Datenbank, Webinterface, optional OLED und optional WS2812B-LEDs.

## Klare Abgrenzung zur Waage/ESP32
- **Waage/ESP32 ist aktiv** und sendet fertige Läufe.
- **Pi ist passiv** und empfängt nur HTTP-Datensätze.
- Der Pi steuert die Waage nicht und fragt keine Messwerte aktiv ab.

## Ordnerstruktur
- `pi/backend`: API, DB, Logik
- `pi/frontend`: HTML/CSS/JS für Dashboard, Läufe, Personen, Status
- `pi/oled`: OLED-Dienst (optional)
- `pi/leds`: LED-Dienst (optional)
- `pi/systemd`: Service-Dateien
- `pi/scripts`: Installation, Dev-Start, Kiosk, API-Test

## Hardwareübersicht & Pinbelegung
- OLED (I2C, 0x3C): SDA GPIO2 (Pin 3), SCL GPIO3 (Pin 5)
- WS2812B: DATA GPIO18 (Pin 12), externe 5V, GND gemeinsam mit Pi
- Empfehlungen WS2812B: Level-Shifter, 330–470Ω Datenleitung, Pufferkondensator
- Touchdisplay: Browser im Kiosk-Modus auf `http://localhost:8000`

## Installation
```bash
cd pi
bash scripts/install.sh
```
Installiert Pakete, erstellt `.venv`, installiert Requirements, legt `data/` + `logs/` an und installiert systemd-Units.

## Entwicklungsmodus
```bash
cd pi
bash scripts/start_dev.sh
```
Startet: `uvicorn backend.main:app --host 0.0.0.0 --port 8000`

## systemd-Installation
```bash
cd pi
bash scripts/create_services.sh
sudo systemctl enable wage-pi-backend wage-pi-oled wage-pi-leds
sudo systemctl start wage-pi-backend wage-pi-oled wage-pi-leds
```
Standardpfad in Service-Dateien: `/home/pi/wage/pi` (bei Bedarf anpassen).

## Kiosk-/Touchdisplay-Start
```bash
cd pi
bash scripts/kiosk_start.sh
```
Sucht automatisch `chromium-browser` oder `chromium` und öffnet `http://localhost:8000` im Kiosk-Modus.

## API-Dokumentation
- Web: `http://<pi-ip>:8000/`
- OpenAPI/Swagger: `http://<pi-ip>:8000/docs`
- Health: `GET /api/v1/health`
- Runs: `POST /api/v1/runs`, `POST /api/v1/runs/batch`, `GET /api/v1/runs`, `PUT /api/v1/runs/{id}`, `DELETE /api/v1/runs/{id}`
- Persons: `GET/POST/PUT/DELETE /api/v1/persons`, `POST /api/v1/persons/{id}/activate`
- Status: `GET /api/v1/status`

## curl-Beispiele
```bash
curl -s http://localhost:8000/api/v1/health

curl -s -X POST http://localhost:8000/api/v1/runs \
  -H 'content-type: application/json' \
  -d '{"protocol_version":"1.0","device_id":"scale-001","boot_id":"boot-abc","run_number":1,"event_id":"evt-1","time_ms":12345,"start_weight_g":87.5,"status":"ok","firmware_version":"0.1.0","queue_depth":0}'

curl -s -X POST http://localhost:8000/api/v1/runs/batch \
  -H 'content-type: application/json' \
  -d '{"runs":[{"protocol_version":"1.0","device_id":"scale-001","boot_id":"boot-abc","run_number":2,"event_id":"evt-2","time_ms":12400,"start_weight_g":88.0,"status":"ok","firmware_version":"0.1.0","queue_depth":0}]}'

curl -s -X POST http://localhost:8000/api/v1/persons \
  -H 'content-type: application/json' \
  -d '{"name":"Max","activate":true}'

curl -s -X POST http://localhost:8000/api/v1/persons/2/activate
```

## Datenbankschema
DB-Datei: `pi/data/wage_pi.sqlite3`
- `persons(id, name UNIQUE, created_at)`
- `devices(device_id UNIQUE, firmware_version, last_seen_at, last_boot_id, last_queue_depth)`
- `runs(..., UNIQUE(device_id, event_id))`
- `app_state(key PRIMARY KEY, value)`

Erststart erzeugt automatisch DB + Standardwerte (`Unbekannt` ID 1, `active_person_id=1`, LED/OLED-Status).

## OLED-Hinweise
- Optional: `luma.oled`, `Pillow`
- Bei fehlender Hardware/Bibliothek kein Absturz; Dienst bleibt im Degraded-Modus und schreibt `app_state.oled_status`.

## LED-Hinweise
- Optional: `rpi_ws281x`
- Bei fehlender GPIO-Rechte/Bibliothek kein Absturz; Dienst bleibt im Degraded-Modus und schreibt `app_state.led_status`.

## Fehlerbehebung
```bash
journalctl -u wage-pi-backend -f
journalctl -u wage-pi-oled -f
journalctl -u wage-pi-leds -f
```

## Testplan
```bash
cd pi
bash scripts/test_api.sh
python3 -m py_compile backend/*.py oled/*.py leds/*.py
```
