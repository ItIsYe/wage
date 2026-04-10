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
  - Skalierung zentral ganz oben im Config-Block (`OLED_SCALE_CONFIG`, akzeptiert z. B. `"1,9"` oder `"1.9"`)
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
  - Warten = grün/blau alternierend
  - Bereit fuer Zeitmessung (`READY_FOR_TIMING`) = nur gruen blinkend
  - Glas erkannt = grün dauerhaft
  - Zeit läuft (`TIMING`) = blaue LEDs laufen im Kreis (oben -> rechts -> unten -> links)
  - Ergebnis (`SHOW_RESULT`) = gruen+blau einmal kurz gemeinsam, danach gruen dauerhaft
  - Standby = sanftes Random-Twinkle

## Prozent-Logik (Start/Stop)

- `readyReferenceWeight` ist das stabile Referenzgewicht des erkannten Glases.
- Beide Schwellen werden direkt als `% vom Referenzgewicht` gerechnet:
  - Start: `max(MIN_DYNAMIC_THRESHOLD_G, ref * START_DROP_PERCENT / 100)`
  - Stop: `max(MIN_DYNAMIC_THRESHOLD_G, ref * STOP_RISE_PERCENT / 100)`
- Damit bleiben Start/Stop robust bei leichten und schweren Gläsern.


## LED-Debug-Modus (separat)

- Der neue Bereich steht oben im Config-/Debug-Teil in `src`.
- Hauptschalter: `LED_DEBUG_MODE`
  - `false` = normaler LED-Betrieb wie bisher
  - `true` = nur manuelle LED-Zustaende, normale LED-Engine ist komplett uebersteuert
- Manuelle Einzelsteuerung pro LED:
  - Gruen:
    - `LED_DEBUG_GREEN_1` = unten rechts
    - `LED_DEBUG_GREEN_2` = oben rechts
    - `LED_DEBUG_GREEN_3` = oben links
    - `LED_DEBUG_GREEN_4` = unten links
  - Blau:
    - `LED_DEBUG_BLUE_1` = mitte oben
    - `LED_DEBUG_BLUE_2` = mitte links
    - `LED_DEBUG_BLUE_3` = mitte rechts
    - `LED_DEBUG_BLUE_4` = mitte unten
  - Rot:
    - `LED_DEBUG_RED_1` = oben links & unten rechts
    - `LED_DEBUG_RED_2` = oben rechts & unten links
- Im Debug-Modus gibt es bewusst keine Blinkmuster und keine zustandsbasierte LED-Automatik.
