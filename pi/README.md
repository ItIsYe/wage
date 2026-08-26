# wage-pi

Raspberry Pi Backend — empfängt Läufe vom ESP32, speichert sie in SQLite, stellt Webinterface und Statusanzeigen bereit.

## Architektur

```
Pi empfängt  ←── ESP32 sendet HTTP POST /api/v1/runs
Pi steuert nicht die Waage
Pi fragt keine Messwerte ab
```

## Services

| Service | Aufgabe |
|---|---|
| `wage-pi-backend` | FastAPI Backend (gunicorn, 2 Worker) |
| `wage-pi-leds` | WS2812B LED-Streifen (322 Pixel) |
| `wage-pi-oled` | OLED Info-Display (I2C) |
| `wage-pi-kiosk` | Chromium Kiosk-Browser |
| `wage-pi-screen-save` | Bildschirm-Schoner + Auto-Shutdown |
| `wage-pi-hw-mgmt` | Hardware Power-Management (Boot) |

## Installation

```bash
cd pi
bash scripts/install.sh
```

Setzt voraus: Raspberry Pi OS, NetworkManager, Python 3.11+.

Einmalig nach der Installation (Boot-Optimierungen):
```bash
sudo bash pi/scripts/setup_boot_config.sh
sudo reboot
```

## Update

Über das Webinterface: **Konfiguration → 5) System-Update → Nach Updates prüfen**.

Oder manuell:
```bash
cd ~/wage
git pull origin beta
/home/wage/wage/pi/.venv/bin/pip install -r pi/requirements.txt -q
sudo install -m 0644 pi/systemd/wage-pi-*.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl restart wage-pi-backend wage-pi-leds wage-pi-oled wage-pi-screen-save wage-pi-kiosk
```

## Hardware

| Komponente | Anschluss |
|---|---|
| WS2812B Strip | GPIO 18, 4 Streifen: 80+80+82+82 = 324 Pixel |
| OLED | I2C Bus 1, Adresse 0x3C (SSD1306 oder SH1106) |
| Touchscreen | DSI oder HDMI, Backlight `/sys/class/backlight/10-0045/brightness` |
| Touch-Input | evdev `10-0038 generic ft5x06` |

## Ordnerstruktur

```
pi/
├── backend/        FastAPI, API-Endpoints, DB
├── frontend/       Jinja2-Templates + CSS/JS
├── leds/           LED-Service + Twinkle-Pattern
├── oled/           OLED-Service
├── screen/         Screen-Save + Auto-Shutdown
├── scripts/        Install, Update, Kiosk, Netzwerk
├── systemd/        Service-Dateien
├── data/           SQLite (wage_pi.sqlite3) — nicht im Repo
└── logs/           Logs — nicht im Repo
```

## API-Endpunkte (Auswahl)

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/v1/health` | Health-Check |
| POST | `/api/v1/runs` | Lauf empfangen |
| GET | `/api/v1/runs` | Läufe abrufen |
| GET | `/api/v1/runs/export/csv` | CSV-Export |
| GET | `/api/v1/status` | System-Status |
| GET | `/api/v1/status/stream` | SSE Live-Stream |
| GET/POST | `/api/v1/config/network` | Netzwerk-Konfiguration |
| GET | `/api/v1/config/network/scan` | WLAN-Scan |
| POST | `/api/v1/config/network/apply` | Netzwerk anwenden |
| GET/POST | `/api/v1/config/leds` | LED-Helligkeiten |
| GET/POST | `/api/v1/config/hardware` | Pi Hardware |
| GET/POST | `/api/v1/system/debug-mode` | Debug-Modus |
| POST | `/api/v1/system/shutdown` | Pi herunterfahren |
| POST | `/api/v1/system/update/apply` | Pi-Code aktualisieren |

Swagger UI: `http://192.168.1.112:8000/docs`

## Netzwerk

### AP-Modus (Standard)
- SSID: `wage-net`
- Pi-IP: `192.168.50.1`
- DHCP: `192.168.50.50–192.168.50.150`
- ESP32 statische IP: `192.168.50.100`
- API-Ziel für ESP: `http://192.168.50.1:8000/api/v1/runs`

### Client-Modus
- Pi verbindet sich mit Haus-WLAN
- WLAN-Scan über Webinterface möglich (auch im AP-Modus)
- Konfiguration über **Konfiguration → 2) Netzwerk-Konfiguration**

### Umschalten
1. Webinterface öffnen → Konfiguration
2. Modus wählen, SSID/Passwort eingeben
3. **Speichern** → **Jetzt anwenden**

## LED-Status

| Farbe/Pattern | Bedeutung |
|---|---|
| Weiß (Boot) | Pi startet |
| Regenbogen | Waage online, Standby |
| Gelb pulsend | Waage offline |
| Grüne Welle | Lauf empfangen |
| Sternenhimmel | Power-Save Modus |
| Rot | Fehler (API/DB) |
| Rot blinkend | Schwerer Fehler |

Helligkeiten konfigurierbar unter **Konfiguration → 3) LED-Konfiguration**.

## Power-Save & Auto-Shutdown

- Nach X Minuten ohne Lauf → Power-Save (konfigurierbar, Standard: 1 Minute)
- Nach 10 Minuten Inaktivität → Bildschirm aus, LEDs Sternenhimmel
- Nach 10 weiteren Minuten → Pi fährt herunter (OLED zeigt Meldung)
- **Debug-Modus** deaktiviert den Auto-Shutdown (unter **Status**)
- Nach dem Neustart setzt `hw_power_mgmt.sh` alle Flags zurück

## Datenbank

SQLite: `pi/data/wage_pi.sqlite3`

Tabellen:
- `runs` — alle Messläufe
- `devices` — bekannte ESP32-Geräte
- `persons` — Personen
- `app_state` — Konfiguration und Status (Key/Value)

Backups werden vor jedem Update automatisch erstellt (max. 10, in `pi/data/backups/`).

## sudoers

Folgende Einträge müssen einmalig gesetzt sein:
```bash
/etc/sudoers.d/wage-shutdown  → sudo shutdown
/etc/sudoers.d/wage-update    → sudo systemctl, sudo install
/etc/sudoers.d/wage-network   → sudo network_apply.sh
```
`install.sh` setzt diese automatisch.

## Fehlerbehebung

| Problem | Lösung |
|---|---|
| Backend startet nicht | `journalctl -u wage-pi-backend -f` |
| LEDs gehen nicht an | `sqlite3 ~/wage/pi/data/wage_pi.sqlite3 "UPDATE app_state SET value='0' WHERE key='led_shutdown';"` dann `sudo systemctl restart wage-pi-leds` |
| OLED nicht erkannt | `i2cdetect -y 1` — Adresse 0x3C prüfen |
| Kiosk zeigt alte Version | `rm -rf /tmp/wage-kiosk && sudo systemctl restart wage-pi-kiosk` |
| Netzwerk anwenden schlägt fehl | `sudo journalctl -u NetworkManager -f` und `cat ~/wage/pi/logs/network_apply.log` |
| Update schlägt fehl | `cat ~/wage/pi/logs/update.log` |
