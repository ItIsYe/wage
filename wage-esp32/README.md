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
```

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

Queue und Retry sind eingebaut — Läufe werden gepuffert wenn der Pi offline ist (max. 50).

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

## FastLED

Gepinnt auf Version `3.7.0` — stabil für WS2812B auf ESP32 mit aktivem WiFi.
Neuere Versionen (3.10+) haben mit dem RMT-Treiber auf dieser Hardware Timing-Probleme gezeigt.

## GitHub Actions

Bei jedem Push auf `beta` der `wage-esp32/**` ändert, wird automatisch gebaut.
Das fertige `.bin` ist über die OTA-Seite im Pi-Webinterface installierbar.
