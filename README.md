# wage

Messsystem für Trinkzeiten — ESP32 Waage + Raspberry Pi Backend.

## Projektstruktur

```
wage/
├── wage-esp32/     ESP32 Firmware (PlatformIO)
└── pi/             Raspberry Pi Backend + UI
```

## Kurzübersicht

| Komponente | Aufgabe |
|---|---|
| ESP32 | Gewicht messen, Lauf erkennen, per HTTP an Pi senden |
| Raspberry Pi | Läufe empfangen, speichern, Webinterface, LED/OLED |

## Branch

Aktiver Entwicklungsbranch: `beta`

## Schnellstart

**ESP32:**
```bash
cd wage-esp32
pio run -t upload
```

**Pi:**
```bash
cd pi
bash scripts/install.sh
```

## Dokumentation

- ESP32: [`wage-esp32/README.md`](wage-esp32/README.md)
- Pi Backend: [`pi/README.md`](pi/README.md)
