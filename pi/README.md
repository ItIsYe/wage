# WAGE Raspberry-Pi-Teil (`/pi`)

Der Pi-Teil liegt **vollständig unter `/pi`**. Der bestehende Waagen-/ESP32-Teil wurde nicht verändert.

## Ziel und Abgrenzung
- Raspberry Pi = Backend, DB, Webinterface, Touch-Kiosk, OLED-/LED-Status.
- Pi ist **passiver Empfänger**: empfängt nur fertige Läufe per HTTP.
- Pi steuert die Waage nicht und fragt keine Messwerte aktiv ab.

## Hardwareübersicht / Pins
- OLED I2C (Adresse `0x3C`): SDA GPIO2 (Pin 3), SCL GPIO3 (Pin 5), VCC, GND.
- WS2812B: Daten GPIO18 (Pin 12).
- LED-Streifen extern mit 5V speisen, GND gemeinsam mit Pi, Level-Shifter empfohlen, 330–470 Ohm Datenwiderstand, Pufferkondensator empfohlen.

## Installation
```bash
cd /home/pi/wage/pi
bash scripts/install.sh
```

## Entwicklung
```bash
cd /workspace/wage/pi
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
bash scripts/start_dev.sh
```

## systemd
- `wage-pi-backend.service`
- `wage-pi-oled.service`
- `wage-pi-leds.service`

Pfadstandard: `/home/pi/wage/pi`. Bei anderem Installationspfad `WorkingDirectory`/`ExecStart` in Service-Dateien anpassen.

## API
- `GET /api/v1/health`
- `POST /api/v1/runs`
- `POST /api/v1/runs/batch`
- `GET /api/v1/runs`
- `GET/POST/PUT/DELETE /api/v1/persons`
- `POST /api/v1/persons/{id}/activate`
- `GET /api/v1/status`
- Swagger: `/docs`

### Beispiel curl: einzelner Lauf
```bash
curl -X POST http://localhost:8000/api/v1/runs -H 'Content-Type: application/json' -d '{
  "protocol_version":"1.0","device_id":"scale-001","boot_id":"boot-abc","run_number":1,
  "event_id":"unique-event-id","time_ms":12345,"start_weight_g":87.5,
  "status":"ok","firmware_version":"0.1.0","queue_depth":0
}'
```

### Beispiel curl: Batch
```bash
curl -X POST http://localhost:8000/api/v1/runs/batch -H 'Content-Type: application/json' -d '{"runs":[{"protocol_version":"1.0","device_id":"scale-001","boot_id":"boot-abc","run_number":1,"event_id":"a","time_ms":100,"start_weight_g":87.5,"status":"ok","firmware_version":"0.1.0","queue_depth":0},{"protocol_version":"1.0","device_id":"scale-001","boot_id":"boot-abc","run_number":2,"event_id":"b","time_ms":101,"start_weight_g":87.6,"status":"ok","firmware_version":"0.1.0","queue_depth":0}]}'
```

## Datenbankschema
SQLite-Datei: `/pi/data/wage_pi.sqlite3`.
Tabellen: `persons`, `devices`, `runs`, `app_state` (gemäß Vorgaben).
Beim ersten Start: Person `Unbekannt` (ID 1) + `active_person_id=1`.

## UI
- Hauptoberfläche unter `/`
- Weitere Seiten: `/runs`, `/persons`, `/status`
- Touch-tauglich (große Buttons, einfache Bedienung)
- Kiosk-Start: `bash scripts/kiosk_start.sh`

## OLED/LED Robustheit
- OLED- und LED-Service laufen separat.
- Fehler dort stoppen das Backend nicht.
- Backend funktioniert ohne OLED/LED/Touchdisplay.

## Fehlerbehebung
- `journalctl -u wage-pi-backend -f`
- DB prüfen: `sqlite3 data/wage_pi.sqlite3 '.tables'`
- Healthcheck: `curl http://localhost:8000/api/v1/health`
