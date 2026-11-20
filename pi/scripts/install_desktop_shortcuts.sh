#!/usr/bin/env bash
# Legt auf dem Pi-Desktop (und im Startmenü) Symbole an:
#   - "Waage-Oberfläche"         startet den Kiosk wieder (nach "Zum Pi-Desktop" in der Oberfläche)
#   - "wage-Dienste neu starten" startet LEDs, OLED, Screen-Save und Backend neu
# Läuft als Desktop-Benutzer (wage). Wird von install.sh und update.sh aufgerufen.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DESK="$(xdg-user-dir DESKTOP 2>/dev/null || true)"
[ -n "$DESK" ] && [ "$DESK" != "$HOME" ] || DESK="$HOME/Desktop"
APPS="$HOME/.local/share/applications"
mkdir -p "$DESK" "$APPS"

write_launcher() {  # Dateiname, Name, Kommentar, Befehl, Symbol, Terminal(true/false)
  local file="$1" content
  content="[Desktop Entry]
Type=Application
Name=$2
Comment=$3
Exec=$4
Icon=$5
Terminal=$6
Categories=Utility;
"
  for dir in "$DESK" "$APPS"; do
    printf '%s' "$content" > "$dir/$file"
    chmod +x "$dir/$file"
    # Desktop-Dateien als vertrauenswürdig markieren, sonst fragt der Dateimanager vor dem Start
    gio set "$dir/$file" metadata::trusted true 2>/dev/null || true
  done
}

write_launcher wage-oberflaeche.desktop "Waage-Oberfläche" \
  "Startet die Waage-Oberfläche (Kiosk) im Vollbild" \
  "sudo -n systemctl start wage-pi-kiosk" "video-display" false

write_launcher wage-dienste-neustarten.desktop "wage-Dienste neu starten" \
  "Startet LEDs, OLED, Bildschirmschoner und Backend neu" \
  "bash $ROOT/scripts/restart_services.sh --pause" "view-refresh" true

echo "Desktop-Symbole angelegt in: $DESK"
