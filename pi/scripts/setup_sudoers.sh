#!/usr/bin/env bash
# Richtet die sudo-Rechte ein, die das Webinterface braucht (Update, Dienste, Netzwerk, Herunterfahren).
# Einmalig ausführen:  sudo bash pi/scripts/setup_sudoers.sh
set -euo pipefail
[ "$(id -u)" = "0" ] || { echo "Bitte mit sudo ausführen: sudo bash $0"; exit 1; }

USER_NAME="${1:-wage}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

# Pfade auf diesem System ermitteln (Debian mit zusammengeführtem /usr: /usr/bin, sonst /bin)
paths() { for n in "$@"; do command -v "$n" || true; done | sort -u | paste -sd, - | sed 's/,/, /g'; }
UPDATE_CMDS="$(paths systemctl install)"
POWER_CMDS="$(paths shutdown reboot)"
NMCLI="$(command -v nmcli || echo /usr/bin/nmcli)"

write_rule() {  # Datei, Inhalt; wird vor dem Aktivieren mit visudo geprüft
    local file="/etc/sudoers.d/$1" tmp
    tmp="$(mktemp)"
    echo "$2" > "$tmp"
    if visudo -cf "$tmp" >/dev/null; then
        install -m 0440 "$tmp" "$file"
        echo "OK  $file: $2"
    else
        echo "FEHLER: Regel für $file ungültig, nicht übernommen"
    fi
    rm -f "$tmp"
}

write_rule wage-update   "$USER_NAME ALL=(ALL) NOPASSWD: $UPDATE_CMDS"
write_rule wage-shutdown "$USER_NAME ALL=(ALL) NOPASSWD: $POWER_CMDS"
write_rule wage-network  "$USER_NAME ALL=(ALL) NOPASSWD: $ROOT/pi/scripts/network_apply.sh, $NMCLI"

echo
echo "Prüfung (als $USER_NAME, ohne Passwort):"
sudo -u "$USER_NAME" sudo -n -l systemctl daemon-reload >/dev/null && echo "  systemctl: ok" || echo "  systemctl: FEHLT"
sudo -u "$USER_NAME" sudo -n -l install --version >/dev/null && echo "  install:   ok" || echo "  install:   FEHLT"
