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

## Timing-Logik

- READY friert eine Referenz (`readyReferenceWeight`) ein und zieht sie nur nach oben nach.
- Start wird bei signifikantem Gewichtsabfall (`START_DROP_G`) mit Hold-Zeit (`DROP_HOLD_MS`) ausgelöst.
- TIMING trackt ein Minimum (`minDuringTiming`).
- Stop wird ausgelöst, wenn das Gewicht vom Minimum wieder signifikant ansteigt (`STOP_RISE_G`) und die Bedingung stabil über `STOP_HOLD_MS` anliegt.
- Nach Ergebnisanzeige wird auf echtes Leerräumen gewartet, danach optional nur im leeren/stabilen Zustand nachnullt (`CHECK_RETARE`).
