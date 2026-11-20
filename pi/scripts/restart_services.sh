#!/usr/bin/env bash
# Startet die wage-Dienste neu: LEDs, OLED, Screen-Save und zuletzt das Backend.
# Aufruf vom Pi-Desktop (Symbol "wage-Dienste neu starten") oder aus der Weboberfläche
# (Status -> Dienste neu starten). Optionen:
#   --with-kiosk  Waage-Oberfläche (Kiosk) ebenfalls neu starten bzw. starten
#   --pause       am Ende 5 s warten (damit das Terminalfenster lesbar bleibt)
set -uo pipefail

WITH_KIOSK=0; PAUSE=0
for arg in "$@"; do
  case "$arg" in
    --with-kiosk) WITH_KIOSK=1 ;;
    --pause) PAUSE=1 ;;
  esac
done

SUDO="sudo -n"
[ "$(id -u)" = "0" ] && SUDO=""

echo "wage-Dienste werden neu gestartet ..."
for svc in wage-pi-leds wage-pi-oled wage-pi-screen-save; do
  if $SUDO systemctl restart "$svc"; then echo "  ok: $svc"; else echo "  FEHLER: $svc"; fi
done
if [ "$WITH_KIOSK" = "1" ]; then
  $SUDO systemctl restart --no-block wage-pi-kiosk && echo "  ok: wage-pi-kiosk (Waage-Oberfläche startet)"
fi
# Backend zuletzt und ohne Warten: Aus der Weboberfläche gestartet läuft dieses Skript im
# Backend-Dienst und würde bei einem blockierenden Neustart mit beendet.
if $SUDO systemctl restart --no-block wage-pi-backend; then
  echo "  ok: wage-pi-backend (startet neu, ca. 5 s)"
else
  echo "  FEHLER: wage-pi-backend"
fi
echo "Fertig. Die LEDs gehen nach dem Neustart erst nach etwa 20 s wieder an."
[ "$PAUSE" = "1" ] && sleep 6
exit 0
