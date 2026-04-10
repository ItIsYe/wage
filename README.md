# wage

## Ablauf der Zustandsmaschine (Soll)

1. `BOOT_MSG`
2. `BOOT_TARE`
3. `IDLE_WAIT_GLASS`
4. `GLASS_DETECTED`
5. `READY_FOR_TIMING`
6. `TIMING`
7. `SHOW_RESULT`
8. `WAIT_EMPTY_AFTER_RESULT`
9. `CHECK_RETARE`
10. zurück zu `IDLE_WAIT_GLASS`

## Vereinfachte Final-Logik

- Es gibt **keine Glasprofile, keine 0.2/0.7-Erkennung und keine Lernlogik** mehr.
- In `IDLE_WAIT_GLASS` wird nur noch unterschieden:
  - Objekt vorhanden (`OBJECT_PRESENT_G` + stabil)
  - kein Objekt vorhanden
- Nach Erkennung folgt kurz `GLASS_DETECTED`, dann `READY_FOR_TIMING`.
- In `READY_FOR_TIMING` startet die Messung bei signifikantem Gewichtsabfall (`START_DROP_PERCENT` vom Referenzgewicht, in g berechnet + `DROP_HOLD_MS`).
- In `TIMING` wird das Minimum getrackt; Stop erfolgt beim signifikanten Anstieg vom Minimum (`STOP_RISE_PERCENT` vom Referenzgewicht, in g berechnet + `STOP_HOLD_MS`).
- Während `TIMING` läuft die Zeit live sichtbar auf dem OLED.
- Nach `SHOW_RESULT` wird gewartet, bis die Waage wirklich leer/stabil ist.
- `CHECK_RETARE` taret nur bei leer + stabil + Offset außerhalb `RETARE_TOL_G`.

## OLED- und LED-Verhalten

- OLED:
  - Skalierung zentral über Config (`OLED_STATUS_TEXT_SCALE`, `OLED_TIME_TEXT_SCALE`)
  - Boot: `Start... / Initialisierung`
  - Nullung: `Nullung... / Bitte nichts auflegen`
  - Idle: `Warte auf Glas`
  - Detected: `Glas erkannt`
  - Ready: `Bereit fuer / Zeitmessung`
  - Timing: Live-Zeit in Sekunden
  - Ergebnis: `Fertig / Zeit: ...`
  - Nach Ergebnis: `Bitte leeren / Glas entfernen`
- LED:
  - Fehler = rot blinkend
  - Warten/Bereit = grün/blau alternierend
  - Glas erkannt = grün dauerhaft
  - Zeit läuft = blau blinkend
  - Standby = sanftes Random-Twinkle
