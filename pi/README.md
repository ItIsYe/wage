# WAGE Raspberry Pi (/pi)

Dieser Bereich betrifft **ausschließlich den Raspberry Pi**. Alles außerhalb von `/pi` wird ignoriert.

## Rolle des Pi
- Pi ist passiver Empfänger und Datenzentrale.
- Waage/ESP32 bleibt aktiver Sender.
- Pi steuert die Waage nicht und fragt keine Messwerte aktiv ab.

## Services
- Backend (`0.0.0.0:8000`)
- LED-Service (separat)
- OLED-Service (separat)

Alle Services starten unabhängig; OLED/LED-Fehler stoppen das Backend nicht.

## Umgebungsvariablen
- `WAGE_PI_LED_COUNT` (Default `8`)
- `WAGE_PI_LED_BRIGHTNESS` (0-255, Default `32`)
- `WAGE_PI_OLED_ADDRESS` (Default `0x3c`)
- `WAGE_PI_URL` (für `scripts/test_api.sh`, Default `http://localhost:8000`)

## LED-Statuslogik
- Weiß: Start
- Blau: Backend läuft, noch kein Waagenkontakt
- Grün: Waage online
- Gelb: Waage offline/kein aktueller Kontakt
- Kurz blau blinkend: neuer Lauf empfangen
- Rot: API/DB-Fehler
- Rot blinkend: schwerer Fehler (`last_event=fatal_error`)
- `degraded:<Fehlerklasse>` bei fehlender Library/GPIO/Hardware

## OLED-Anzeige
- Treiber: SSD1306 bevorzugt, SH1106 Fallback
- Anzeige: Titel, URL/IP, aktive Person, Waage online/offline, letzter Kontakt
- QR-Code (wenn `qrcode` + `pillow` verfügbar und Display geeignet), sonst Klartext-URL
- Bei Fehlern: `degraded:<Fehlerklasse>` statt Absturz

## Touchdisplay/Kiosk
`bash scripts/kiosk_start.sh` öffnet Chromium im Kiosk-Modus. UI ist touchfreundlich mit großen Karten/Buttons/Schrift.

## Testskript
`bash scripts/test_api.sh`
- Healthcheck
- Person anlegen und aktiv setzen
- Lauf senden + Duplikat senden
- Prüft `duplicate=true`
- Läufe + Status abrufen

Optional:
```bash
WAGE_PI_URL=http://192.168.1.116:8000 bash scripts/test_api.sh
```

## systemd
Standardpfad in Unit-Dateien: `/home/pi/wage/pi` (bei anderer Repo-Lage anpassen).

## Fehlerbehebung
- OLED nicht erkannt: I2C aktivieren (`raspi-config`), Adresse prüfen (`i2cdetect -y 1`), `WAGE_PI_OLED_ADDRESS` setzen.
- LED braucht Rechte: Service mit passenden GPIO-Rechten/Root starten.
- `rpi_ws281x` fehlt: `pip install -r requirements.txt`.
- `luma.oled` fehlt: `pip install -r requirements.txt`.
- API läuft nicht: `journalctl -u wage-pi-backend -f`.
- Datenbank fehlt/leer: Backend einmal starten, Datei unter `pi/data/wage_pi.sqlite3` prüfen.
