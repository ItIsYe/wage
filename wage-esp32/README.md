# wage-esp32

Kurzüberblick für Build, Flash und Hardwaretest der modularen Waage.

## Build

```bash
cd wage-esp32
pio run
```

## Flash

```bash
cd wage-esp32
pio run -t upload
```

## Webinterface

- Access Point: **Waage-Config**
- Konfigurationsseite im Browser öffnen (AP-IP).
- Alle Debug-Modi deaktivieren über: `/debug/off`

## LED-Ringe

- Ring 1: GPIO **5**, **25** Pixel
- Ring 2: GPIO **14**, **16** Pixel

### Ring-2 Unabhängigkeitstest

Für den Hardware-Diagnosetest in `include/config.h` setzen:

```cpp
static constexpr bool RING2_FORCE_INDEPENDENT_TEST = true;
```

Erwartung nach Flash:

- Ring 1 läuft normal weiter (unverändert)
- Ring 2 zeigt fest:
  - Pixel 0 = Rot
  - Pixel 1 = Grün
  - Pixel 2 = Blau
  - Pixel 3 = Weiß
  - alle anderen aus

Wenn Ring 2 in diesem Modus weiterhin Ring 1 spiegelt, liegt das Problem sehr wahrscheinlich bei Flash/Pin/Hardware und nicht in der Pattern-Logik.

Danach für Normalbetrieb wieder zurücksetzen:

```cpp
static constexpr bool RING2_FORCE_INDEPENDENT_TEST = false;
```

Ring-2 Webinterface-Modi für Funktionstest:

- Pattern 0: aus
- Pattern 1: Solid Blau
- Pattern 2: Pulse Blau
- Debug: alle Pixel an

## Externe Schnittstelle

- Versand per **HTTP POST**
- Default API-Pfad: `/api/v1/runs`
- Queue/Retry ist integriert
- Standalone-Betrieb funktioniert auch ohne Zielgerät

## Relevante JSON-Felder

- `event_id`
- `device_id`
- `boot_id`
- `run_number`
- `time_ms`
- `start_weight_g`
- `min_weight_g`
- `start_drop_threshold_g`
- `stop_rise_threshold_g`
