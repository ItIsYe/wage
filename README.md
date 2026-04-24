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
- Die Stop-Kandidatenlogik in `TIMING` arbeitet mit kurzer Reset-Hysterese (`STOP_RESET_HYST_G`), damit kurze Messlücken/Rauschen den Stop nicht ständig zurücksetzen.
- Während `TIMING` läuft die Zeit live sichtbar auf dem OLED.
- Nach `SHOW_RESULT` wird gewartet, bis die Waage wirklich leer/stabil ist.
- `CHECK_RETARE` taret nur bei leer + stabil + Offset außerhalb `RETARE_TOL_G`.

## OLED- und LED-Verhalten

- OLED:
  - Display-Treiber: **SH1106 (I2C, 128x64)** via `Adafruit_SH110X`
  - Rotation kommt aus `OLED_ROTATION` (0..3)
  - Skalierung zentral ganz oben im Config-Block (`OLED_SCALE_CONFIG`, akzeptiert z. B. `"1,9"` oder `"1.9"`)
  - Boot: `Start... / Initialisierung`
  - Nullung: `Nullung... / Bitte nichts auflegen`
  - Idle: `Warte auf Glas`
  - Detected: `Glas erkannt`
  - Ready: `Bereit fuer / Zeitmessung`
  - Timing: Live-Zeit in Sekunden (bei 128x64 mit größerer Zeitzeile für bessere Lesbarkeit)
  - Ergebnis: `Fertig / Zeit: ...`
  - Nach Ergebnis: `Bitte leeren / Glas entfernen`
  - Debug-OLED (`DEBUG_MODE`): zeigt Rohwerte, Mittelwert, Filter, Stabilität, State, Objektstatus und Fehlercode
- LED:
  - 25 einzeln adressierbare Pixel (`PIXEL_COUNT = 25`)
  - Keine Segment- und keine Positionslogik
  - Keine alte Einzel-LED-Gruppenlogik mehr
  - Fehler = rot blinkend
  - Warten = Gruppe A/B alternierend in grün/blau (gerade/ungerade Pixel)
  - Bereit fuer Zeitmessung (`READY_FOR_TIMING`) = nur gruen blinkend
  - Glas erkannt = alle Pixel grün dauerhaft
  - Zeit läuft (`TIMING`) = blauer Pixel-Spinner direkt über Pixelindex
  - Ergebnis (`SHOW_RESULT`) = Gruppe A grün + Gruppe B blau, kurz
  - Standby = ruhiges Twinkle über wenige zufällige Pixel + seltener roter Akzent

## Prozent-Logik (Start/Stop)

- `readyReferenceWeight` ist das stabile Referenzgewicht des erkannten Glases.
- Beide Schwellen werden direkt als `% vom Referenzgewicht` gerechnet:
  - Start: `max(MIN_DYNAMIC_THRESHOLD_G, ref * START_DROP_PERCENT / 100)`
  - Stop: `max(MIN_DYNAMIC_THRESHOLD_G, ref * STOP_RISE_PERCENT / 100)`
- Damit bleiben Start/Stop robust bei leichten und schweren Gläsern.


## Pixel-Debug und Helligkeit

- `PIXEL_DEBUG_ALL_ON`
  - `false` = normaler Pixel-Betrieb
  - `true` = alle 25 Pixel dauerhaft an (übersteuert Muster nur für Debug)
- `PIXEL_BRIGHTNESS_PERCENT`
  - Wertebereich 0..100 %
  - Wird intern sauber auf 0..255 für `setBrightness()` umgerechnet
