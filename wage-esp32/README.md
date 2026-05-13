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
- Ring 2: GPIO **27**, **16** Pixel

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
