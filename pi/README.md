# wage-pi (nur Raspberry-Pi-Code unter `/pi`)

> **Wichtig:** Dieses Verzeichnis enthält ausschließlich den Raspberry-Pi-Teil. Alles außerhalb von `/pi` wird ignoriert und nicht verändert.

## Projektziel
Der Pi ist lokale Datenzentrale und UI:
- HTTP-Backend (FastAPI)
- SQLite-Datenbank
- Haupt-Webinterface (Dashboard, Läufe, Personen, Status)
- Touchdisplay/Kiosk-Oberfläche
- kleines OLED-Info-Display
- WS2812B-LED-Statusanzeige
- Betrieb als systemd-Services

## Abgrenzung zur Waage
- Der Pi ist **passiver Empfänger**.
- Die Waage/ESP32 ist **aktiver Sender**.
- Der Pi steuert die Waage **nicht**.
- Der Pi fragt Messwerte **nicht aktiv** ab.
- Der Pi empfängt fertige Läufe per HTTP (`POST /api/v1/runs`).

## Ordnerstruktur
- `backend/`: FastAPI, API-Endpoints, DB-Init
- `frontend/`: Jinja2-Templates + statische Dateien
- `oled/`: OLED-Service (degraded-fähig)
- `leds/`: LED-Service (degraded-fähig)
- `scripts/`: Installation, Dev-Start, Kiosk, API-Test
- `systemd/`: Unit-Dateien
- `data/`: SQLite-Datei (`wage_pi.sqlite3`)
- `logs/`: lokale Logs

## Hardwareübersicht
- Raspberry Pi (empfohlen mit LAN/WLAN)
- OLED I2C (SSD1306 bevorzugt, SH1106 Fallback)
- WS2812B LED-Streifen
- optional Touchdisplay mit Chromium Kiosk

## Pinbelegung OLED
- Bus: I2C (`/dev/i2c-1`)
- Standardadresse: `0x3C` (env überschreibbar)
- 3.3V, GND, SDA, SCL nach Standard-Pi-I2C-Belegung

## Pinbelegung WS2812B
- Datenpin: GPIO18
- Stromversorgung extern und korrekt mit GND-Referenz
- Helligkeit absichtlich niedrig voreingestellt

## Touchdisplay / Kiosk
Script:
```bash
bash scripts/kiosk_start.sh
```
Verwendet `WAGE_PI_KIOSK_URL` (Default `http://localhost:8000`).

### Touchscreen-Eingabe
- `scripts/kiosk_start.sh` startet **keine dauerhaft sichtbare OS-Tastatur** (`squeekboard`, `matchbox-keyboard`, `onboard` werden nicht im Hintergrund erzwungen).
- Chromium wird mit Touch-/Virtual-Keyboard-freundlichen Flags gestartet (`--touch-events=enabled`, `--enable-features=VirtualKeyboard`, `--disable-features=TranslateUI`).
- Je nach Raspberry-Pi-OS, X11/Wayland und Chromium-Version kann die native OS-Tastatur automatisch erscheinen – das ist in Ordnung, aber keine harte Abhängigkeit.
- Zusätzlich gibt es im Pi-Webinterface eine integrierte Fallback-Tastatur: Sie erscheint nur bei aktiven Eingabefeldern und verschwindet nach **OK** oder Klick außerhalb.
- Falls eine externe/native Tastatur stört, keine OS-Tastatur im `kiosk_start.sh` dauerhaft starten.

## Installation
```bash
cd pi
bash scripts/install.sh
```
Installiert Pakete, erstellt `.venv`, installiert Python-Requirements, legt Verzeichnisse an, installiert/aktiviert systemd-Units.

## Entwicklungsstart
```bash
cd pi
bash scripts/start_dev.sh
```
Startet `uvicorn backend.main:app --host 0.0.0.0 --port 8000`.

## systemd-Installation
```bash
cd pi
bash scripts/create_services.sh
sudo systemctl enable wage-pi-backend wage-pi-oled wage-pi-leds
```
Standard-WorkingDirectory in Units: `/home/pi/wage/pi`.

## Services bedienen
```bash
sudo systemctl start wage-pi-backend wage-pi-oled wage-pi-leds
sudo systemctl stop wage-pi-backend wage-pi-oled wage-pi-leds
sudo systemctl status wage-pi-backend wage-pi-oled wage-pi-leds
journalctl -u wage-pi-backend -f
```

## API-Dokumentation
- Swagger UI: `GET /docs`
- Health: `GET /api/v1/health`
- Läufe: `POST /api/v1/runs`, `POST /api/v1/runs/batch`, `GET /api/v1/runs`, `PUT/DELETE /api/v1/runs/{id}`
- Personen: `GET/POST/PUT/DELETE /api/v1/persons`, `POST /api/v1/persons/{id}/activate`
- Status: `GET /api/v1/status`

## curl-Beispiele
```bash
# health
curl -s http://localhost:8000/api/v1/health | jq

# Lauf senden
curl -s -X POST http://localhost:8000/api/v1/runs \
  -H 'content-type: application/json' \
  -d '{"protocol_version":"1.0","device_id":"scale-001","boot_id":"boot-abc","run_number":1,"event_id":"unique-event-id","time_ms":12345,"start_weight_g":87.5,"status":"ok","firmware_version":"0.1.0","queue_depth":0}' | jq

# Batch senden
curl -s -X POST http://localhost:8000/api/v1/runs/batch \
  -H 'content-type: application/json' \
  -d '{"runs":[{"protocol_version":"1.0","device_id":"scale-001","boot_id":"boot-abc","run_number":1,"event_id":"evt-1","time_ms":123,"start_weight_g":10.0,"status":"ok","firmware_version":"0.1.0","queue_depth":0}]}' | jq

# Person anlegen
curl -s -X POST http://localhost:8000/api/v1/persons -H 'content-type: application/json' -d '{"name":"Max","activate":false}' | jq

# Person aktiv setzen
curl -s -X POST http://localhost:8000/api/v1/persons/2/activate | jq

# Status abrufen
curl -s http://localhost:8000/api/v1/status | jq
```

## Datenbankschema
SQLite-Datei: `pi/data/wage_pi.sqlite3`
- `persons`
- `devices`
- `runs` (inkl. `UNIQUE(device_id, event_id)`)
- `app_state`

## LED-Statuslogik
- Weiß: Start
- Blau: Backend ok, noch kein Waagenkontakt
- Grün: Waage online
- Gelb: Waage offline/kein aktueller Kontakt
- Kurz blau blinkend: neuer Lauf
- Rot: API/DB-Fehler
- Rot blinkend: schwerer Fehler
- `degraded:<Fehlerklasse>` bei fehlender Lib/GPIO/Hardware

## OLED-Anzeige
- Titel `wage-pi`
- URL/IP
- aktive Person
- Waage online/offline + Kontaktstatus
- API/DB-Status
- QR-Code wenn möglich, sonst Klartext-URL
- bei Fehlern: degraded-Status, kein Backend-Absturz

## Umgebungsvariablen
- `WAGE_PI_LED_COUNT`
- `WAGE_PI_LED_BRIGHTNESS`
- `WAGE_PI_OLED_ADDRESS`
- `WAGE_PI_URL` (Testskript)
- `WAGE_PI_KIOSK_URL`
- `WAGE_PI_OFFLINE_THRESHOLD_SECONDS`

## Testskript
```bash
cd pi
bash scripts/test_api.sh
```
Prüft Health, Personen, Aktivierung, Laufannahme, Duplikaterkennung, Runs, Status inkl. LED/OLED-Felder.

## Fehlerbehebung
- API läuft nicht: `journalctl -u wage-pi-backend -f`
- DB fehlt: Backend starten, `pi/data/wage_pi.sqlite3` prüfen
- OLED nicht erkannt: I2C aktivieren, Adresse prüfen (`i2cdetect -y 1`)
- LED benötigt Rechte: Service mit passenden GPIO-Rechten starten
- `rpi_ws281x` fehlt: `pip install -r requirements.txt`
- `luma.oled` fehlt: `pip install -r requirements.txt`
- I2C nicht aktiv: per `raspi-config` aktivieren
- Port 8000 belegt: Prozess beenden oder Port ändern

## Testplan
1. Backend starten (`start_dev.sh`)
2. `GET /api/v1/health`
3. Lauf senden (`POST /api/v1/runs`)
4. denselben Lauf erneut senden (`duplicate=true`)
5. `GET /api/v1/runs`
6. Personen anlegen/aktivieren
7. neuen Lauf senden und Personzuordnung prüfen
8. `GET /api/v1/status` auf Pflichtfelder prüfen
9. `/`, `/runs`, `/persons`, `/status`, `/docs` im Browser prüfen
10. `scripts/test_api.sh` erfolgreich ausführen


## Netzwerk-Konfiguration (AP oder Haus-WLAN)
- Neue Seite: `/config` (Navigation: **Konfiguration**)
- Modus **AP** (Standard): SSID `wage-net`, Pi-IP `192.168.50.1`, DHCP-Range `192.168.50.50-192.168.50.150`
- Pi-AP ist für ESP32-Kompatibilität fest auf **2.4 GHz** mit `band=bg` und `channel=6` konfiguriert.
- AP-Security ist fest auf **WPA2-PSK / RSN / CCMP** gesetzt (kein WPA3/SAE, kein TKIP, kein Mischmodus).
- PMF ist standardmäßig **optional** (`pmf=1`); falls ein ESP32 weiterhin nicht verbindet, kann testweise `pmf=0` gesetzt werden.
- API-Ziel für ESP im AP-Modus: `http://192.168.50.1:8000/api/v1/runs`
- Modus **Client**: Pi verbindet sich mit Haus-WLAN (für Updates/Internet/Administration)
- Speichern über `POST /api/v1/config/network`, Anwenden über `POST /api/v1/config/network/apply`
- Status über `GET /api/v1/config/network/status`
- Passwörter werden in GET-Antworten nie im Klartext ausgegeben, nur `*_password_set`
- Anwenden benötigt `nmcli` (NetworkManager).

### Risiken beim Anwenden
- Netzwerkwechsel kann die aktuelle Webverbindung kurzzeitig trennen.
- Nach Änderungen wird ein Neustart empfohlen.

### Fehlerbehebung Netzwerk
- `nmcli fehlt`: NetworkManager installieren/aktivieren (`sudo apt install network-manager`).
- `WLAN-Interface nicht gefunden`: `nmcli device status` prüfen, WLAN-Hardware/Driver prüfen.
- `AP startet nicht`: `journalctl -u NetworkManager -f` und `pi/logs/network_apply.log` prüfen.
- `Client verbindet nicht`: SSID/Passwort kontrollieren, Reichweite/Kanal prüfen.
- `Pi nicht mehr erreichbar`: per LAN einloggen oder lokal am Pi auf `/config` zurückstellen.

## Pi-Updates (nur `/pi`)

Der Web-Updater ist ein **Pi-Code-Sync** mit Migrationsschritten.
Quelle der Wahrheit für Pi-Code ist **`origin/beta:/pi`**.

### Sync-Verhalten
- Beim Prüfen (`POST /api/v1/system/update/check`) wird `origin/beta` gefetched und ein Dateivergleich auf Pi-Code-Pfaden durchgeführt.
- Lokale Pi-Code-Abweichungen blockieren nicht mehr; sie werden beim Update durch den Repo-Stand ersetzt.
- Nicht mehr im Remote vorhandene Pi-Code-Dateien werden lokal entfernt.
- Geschützte Runtime-Daten bleiben immer unangetastet:
  - `pi/data/`
  - `pi/logs/`
  - Datenbank (`pi/data/wage_pi.sqlite3`, `-shm`, `-wal`)
  - Netzwerk-Konfiguration (`pi/data/network_config.json`)
  - Logs (`pi/logs/network_apply.log`)
  - `pi/**/__pycache__/`, `pi/**/*.pyc`

### Web-Update im `/config`-Bereich
- Standardansicht: Statuskarte + **„Nach Updates prüfen“**.
- Ohne Sync-Bedarf: **„Keine Pi-Updates verfügbar“**.
- Mit Sync-Bedarf: **„Pi-Code-Sync verfügbar“** mit Listen für:
  - neue/geänderte Dateien
  - Dateien, die entfernt werden
- Hinweise in der UI:
  - „Lokale Betriebsdaten bleiben geschützt.“
  - „Lokale Pi-Code-Abweichungen werden durch den Repo-Stand ersetzt.“
- Update bleibt im AP-Modus gesperrt: nur im Haus-WLAN-Client-Modus.

### Update-Ablauf (`POST /api/v1/system/update/apply`)
1. Update wird vorbereitet
2. Pi-Code wird synchronisiert
3. Alte Pi-Code-Dateien werden entfernt
4. Konfiguration wird migriert
5. Abhängigkeiten werden geprüft
6. Services werden neu gestartet
7. Update abgeschlossen

Danach wird das Backend zuletzt neu gestartet.

### Config-Migration
- Nach Code-Sync läuft `ensure_config_defaults()`.
- Neue Config-Keys werden ergänzt.
- Bestehende lokale Werte (WLAN/AP/Client) bleiben erhalten.
- `network_config.json` wird sicher aus `app_state` neu erzeugt.
- Passwörter werden nicht im Klartext ausgegeben.

### API-Endpunkte für Update
- `GET /api/v1/system/update/status`
- `POST /api/v1/system/update/check`
- `POST /api/v1/system/update/apply`


### AP/Client-Passwort-Handling
- Pi-AP nutzt WPA/WPA2-PSK; AP-Passwort muss mindestens 8 Zeichen haben.
- Passwörter werden in `app_state` der SQLite-DB `pi/data/wage_pi.sqlite3` gespeichert.
- `pi/data/network_config.json` enthält keine Klartext-Passwörter, nur `*_password_set`.
- `pi/scripts/network_apply.sh` liest Secrets direkt aus SQLite (nicht aus JSON).

### nmcli-Debug-Befehle
```bash
nmcli connection show
nmcli connection show wage-net-ap
journalctl -u NetworkManager -f
tail -f pi/logs/network_apply.log
```

### Netzwerk-Fehlerbehebung (AP/Client)
- ESP verbindet nicht: SSID/Passwort prüfen, `network_apply.log` und NetworkManager-Logs prüfen.
- Passwort zu kurz: AP-Passwort muss mind. 8 Zeichen haben, sonst wird Apply abgebrochen.
- `nmcli` fehlt: `sudo apt install network-manager` und NetworkManager aktivieren.
- `sqlite3` fehlt: `sudo apt install sqlite3`.
- AP aktiv, aber kein DHCP: `ipv4.method shared` in Connection prüfen und Connection neu anwenden.
- Falsche SSID/Passwort: Konfiguration auf `/config` korrigieren und neu anwenden.
