# wage-esp32

ESP32 Firmware für die Waage — misst Trinkzeiten und sendet Läufe an den Raspberry Pi.

## Hardware

| Komponente | Pin | Wert |
|---|---|---|
| HX711 #1 DOUT | GPIO 32 | — |
| HX711 #1 SCK | GPIO 33 | — |
| HX711 #2 DOUT | GPIO 25 | — |
| HX711 #2 SCK | GPIO 26 | — |
| LED Ring 1 | GPIO 5 | 160 Pixel, WS2812B |
| LED Ring 2 | GPIO 14 | 24 Pixel, WS2812B |
| OLED SDA | GPIO 21 | I2C, 0x3C |
| OLED SCL | GPIO 22 | I2C, 0x3C |

## Tests

Host-Tests ohne Hardware (Lauf-Modul und Queue):
```bash
cd wage-esp32
pio test -e native
```

## Build & Flash

```bash
cd wage-esp32

# Lokal flashen
pio run -t upload

# OTA über WLAN (ESP muss laufen, PC im selben Netz)
pio run -e esp32dev-ota -t upload
```

## Konfiguration

Alle Einstellungen in `include/config.h`. Wichtigste Werte:

```cpp
PIXEL_COUNT = 160           // Ring 1 Pixel
RING2_PIXEL_COUNT = 24      // Ring 2 Pixel
HEARTBEAT_INTERVAL_MS = 30000   // Heartbeat alle 30s
OFFLINE_PROBE_INTERVAL_MS = 60000  // Pi-Probe alle 60s wenn offline
STANDBY_AFTER_MS = 25000    // Standby nach 25s ohne Glas
TIMING_MAX_MS = 240000      // Messung nach 240s ohne Stop abbrechen
```

**Zeitlimit:** Wird das Glas angehoben und nicht innerhalb von 240 s zurückgestellt, bricht die Waage die Messung ab (OLED „Abbruch / Zeitlimit“, Ring 1 blinkt rot). Der Lauf wird verworfen und nicht an den Pi gesendet; danach geht es über „Bitte leeren“ zurück in den Leerlauf.

## Webinterface

Im AP-Modus erreichbar unter der AP-IP:
- Konfiguration: Kalibrierung, Schwellwerte, Netzwerk
- Debug: LED-Modi, Pixel-Test, Logs
- OTA: Firmware-Update vom Pi

## Netzwerk

- **AP-Modus** (Standard): SSID `Waage-Config`, Passwort `waagecfg1`
- **STA-Modus**: verbindet sich mit `wage-net` (Pi-AP) oder Haus-WLAN
- Statische IP im `wage-net`: `192.168.50.100`
- API-Ziel: `http://192.168.50.1:8000/api/v1/runs`

## LED-Patterns

| Pattern | Bedeutung |
|---|---|
| Rot (Boot) | Initialisierung |
| Blau Spinner | Tarierung |
| Grün/Blau alternierend | Warte auf Glas |
| Blau Spinner (schnell) | Messung läuft |
| Grün + Blau Flash | Lauf erfolgreich |
| Sternenhimmel (Twinkle) | Standby |
| Rot blinkend | Fehler |

## Externe Schnittstelle

Sendet Läufe per `HTTP POST /api/v1/runs` an den Pi:

```json
{
  "device_id": "scale-001",
  "boot_id": "...",
  "run_number": 1,
  "event_id": "unique-id",
  "time_ms": 12345,
  "start_weight_g": 335.5,
  "min_weight_g": 12.3,
  "start_drop_threshold_g": 16.8,
  "stop_rise_threshold_g": 16.8,
  "status": "ok",
  "firmware_version": "beta-abc1234",
  "queue_depth": 0
}
```

Queue und Retry sind eingebaut — Läufe werden gepuffert wenn der Pi offline ist (max. 50):
- Die neuesten 20 Läufe überleben einen Neustart und ein OTA-Update (Flash). Nach einem Neustart fehlt ihnen das Alter (`age_ms`).
- Antwortet der Pi mit einem endgültigen Fehler (HTTP 4xx, z. B. 422), wird der Lauf verworfen statt die Queue zu blockieren. 5xx und Verbindungsfehler werden wiederholt.
- `age_ms` im Lauf sagt dem Pi, wie lange die Messung zurückliegt: Er ordnet den Lauf der Person zu, die **zur Messzeit** aktiv war.
- Die Boot-ID ist ein dauerhafter Zähler (Startwert beim ersten Start zufällig); die Event-ID `<device>-<boot>-<lauf>` ist damit auch nach Neustarts eindeutig.

## Diagnose

**Ring 2 Unabhängigkeitstest** — in `config.h` setzen:
```cpp
static constexpr bool RING2_FORCE_INDEPENDENT_TEST = true;
```
Erwartung: Ring 1 normal, Ring 2 zeigt Pixel 0=Rot, 1=Grün, 2=Blau, 3=Weiß.
Nach dem Test wieder auf `false` setzen.

**Debug-Log** — in `config.h`:
```cpp
static constexpr bool MASTER_DEBUG_LOG = true;
```
Gibt alle Zustände über Serial aus. Für Produktion auf `false`.

**Ring-1-Pixeldiagnose** — im Webinterface (Debug-Bereich) die Checkbox „Ring 1 Diagnose“ aktivieren:
Der Strip schaltet jeden physischen Pixel einzeln nacheinander ein (~400 ms, dezentes Weiß) und gibt den Index über Serial aus.
So lässt sich ablesen, ab welchem Pixel das Signal abbricht. Wird nicht gespeichert (nach Neustart wieder aus).

## Build-Umgebung

Plattform und Bibliotheken sind in `platformio.ini` fest gepinnt (`espressif32@7.1.3`, Arduino-Core 2.0.17).
Ohne Pin zieht PlatformIO die jeweils neueste Plattform — mit dem neueren Core passt die Firmware nicht mehr in die Standard-OTA-Partition (1 310 720 Bytes) und der Build bricht ab.
Ein Plattform-Update daher bewusst vornehmen und die Flash-Auslastung im Build-Log prüfen.

## FastLED

Gepinnt auf Version `3.7.0` — stabil für WS2812B auf ESP32 mit aktivem WiFi.
Neuere Versionen (3.10+) haben mit dem RMT-Treiber auf dieser Hardware Timing-Probleme gezeigt.
Build-Flag: `FASTLED_INTERRUPT_RETRY_COUNT=8` (Neuübertragung bei durch WiFi-Interrupts gestörten Frames).
`FASTLED_ALLOW_INTERRUPTS=0` nicht setzen — wird von FastLED auf ESP32 abgelehnt.

## LED-Code

- Ring 1 und Ring 2 rendern unabhängig voneinander, jeder mit eigenem Standby-Sternenhimmel (kein gemeinsamer „Shared Standby“ mehr).
- Das „Warte auf Glas“-Muster (abwechselnd gerade/ungerade Dreiergruppen) wird zur Laufzeit aus `PIXEL_GROUPS` berechnet — Änderungen an `PIXEL_COUNT` brauchen keine Anpassung von Arrays.
- Beim Boot: 300 ms Wartezeit und zwei `FastLED.show()`-Aufrufe, damit die Versorgung stabil ist.

## Hardware-Hinweise LEDs

- Datenleitungen über Pegelwandler **SN74AHCT125N** (je Ring ein Gatter): 3,3 V → 5 V, `OE` auf GND.
- 5-V-Versorgung am gemeinsamen Verzweigungspunkt mit Pufferkondensatoren (4 × 1000 µF parallel, Polarität beachten).
- Gemeinsame Versorgung für ESP32, Ring 1 und Ring 2; LED-Helligkeit wegen Powerbank (22 W) begrenzt.
- **Erfahrung:** Wenn immer dieselben Pixel ab einer festen Stelle dunkel bleiben (unabhängig von Muster, Firmware und Versorgung), die Verbindungsstellen des Strips prüfen bzw. nachlöten. Ursache bei Ring 1 waren schwache Lötstellen.

## GitHub Actions

Bei jedem Push auf `beta` der `wage-esp32/**` ändert, wird automatisch gebaut.
Das fertige `.bin` ist über die OTA-Seite im Pi-Webinterface installierbar.
Bei Pull Requests wird ebenfalls gebaut (Kompilier- und Größenprüfung), aber kein Release veröffentlicht.
