#!/usr/bin/env bash
# /home/wage/wage/pi/scripts/update.sh
# Vollständiger Update-Prozess für wage-pi
# Wird vom Pi-Webinterface aufgerufen oder manuell: bash pi/scripts/update.sh

set -Eeuo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PI="$ROOT/pi"
LOG="$PI/logs/update.log"
BACKUP_DIR="$PI/data/backups"
DB="$PI/data/wage_pi.sqlite3"
PY="$PI/.venv/bin/python3"

mkdir -p "$PI/logs" "$BACKUP_DIR"

log() { echo "[$(date -Iseconds)] $*" | tee -a "$LOG"; }
fail() { log "FEHLER: $*"; exit 1; }

# Update-Zustand für die Weboberfläche in die DB schreiben (gleiche Keys wie backend/api_update.py,
# gemeinsames Modul backend/update_state.py). Fehler hier dürfen das Update nicht abbrechen.
# Aufruf: set_update_state <ui_state> <status> <ui_message> <progress_step> <progress_percent>
set_update_state() {
    (cd "$PI" && "$PY" -m backend.update_state "$@") >/dev/null 2>&1 || true
}

progress() { set_update_state updating "installiere update..." "Pi-Update wird durchgeführt..." "$1" "$2"; }

on_error() {
    local rc=$? line=$1
    trap - ERR
    log "FEHLER: Update abgebrochen (Zeile $line, Exit-Code $rc)"
    set_update_state error "update fehlgeschlagen" "Update fehlgeschlagen (Zeile $line) – Details in pi/logs/update.log" "Update fehlgeschlagen" 100
    exit "$rc"
}
trap 'on_error $LINENO' ERR

log "=== wage-pi Update gestartet ==="

# === 1. DB-Backup ===
log "1/7 DB-Backup..."
progress "DB-Backup..." 10
if [ -f "$DB" ]; then
    BACKUP="$BACKUP_DIR/wage_pi_$(date +%Y%m%d_%H%M%S).sqlite3"
    cp "$DB" "$BACKUP"
    log "  Backup: $BACKUP"
    # Alte Backups aufräumen (max. 10 behalten)
    ls -t "$BACKUP_DIR"/wage_pi_*.sqlite3 2>/dev/null | tail -n +11 | xargs rm -f 2>/dev/null || true
    log "  Alte Backups bereinigt (max. 10)"
fi

# === 2. git pull ===
log "2/7 Code aktualisieren..."
progress "Code wird aktualisiert..." 20
cd "$ROOT"
git fetch origin beta 2>&1 | while read l; do log "  git: $l"; done
git reset --hard origin/beta 2>&1 | while read l; do log "  git: $l"; done
sudo chown -R wage:wage "$ROOT"
log "  Code aktuell: $(git log --oneline -1)"

# === 3. Python-Pakete ===
log "3/7 Python-Pakete aktualisieren..."
progress "Python-Pakete werden aktualisiert..." 35
"$PI/.venv/bin/pip" install -q -r "$PI/requirements.txt" --upgrade 2>&1 | tail -5 | while read l; do log "  pip: $l"; done
log "  Pakete aktuell"

# === 4. Systemd Services installieren ===
log "4/7 Systemd Services aktualisieren..."
progress "Dienste werden installiert..." 60
SERVICES=(wage-pi-backend wage-pi-leds wage-pi-oled wage-pi-kiosk wage-pi-screen-save wage-pi-hw-mgmt)
for svc in "${SERVICES[@]}"; do
    SVC_FILE="$PI/systemd/${svc}.service"
    if [ -f "$SVC_FILE" ]; then
        sudo install -m 0644 "$SVC_FILE" /etc/systemd/system/
        log "  Installiert: ${svc}.service"
    fi
done
sudo systemctl daemon-reload

# Nicht mehr vorhandene Services deaktivieren
for f in /etc/systemd/system/wage-pi-*.service; do
    SVC_NAME="$(basename "$f")"
    if [ ! -f "$PI/systemd/$SVC_NAME" ]; then
        log "  Entfernt veralteter Service: $SVC_NAME"
        sudo systemctl stop "$SVC_NAME" 2>/dev/null || true
        sudo systemctl disable "$SVC_NAME" 2>/dev/null || true
        sudo rm -f "$f"
    fi
done
sudo systemctl daemon-reload

# === 5. NM Dispatcher ===
log "5/7 NetworkManager Dispatcher..."
progress "NetworkManager-Dispatcher..." 70
if [ -f "$PI/scripts/nm_dispatcher_eth_route.sh" ]; then
    sudo install -m 0755 "$PI/scripts/nm_dispatcher_eth_route.sh" /etc/NetworkManager/dispatcher.d/99-wage-eth-route
    log "  NM-Dispatcher aktualisiert"
fi

# === 6. DB-Migration und Config-Defaults ===
log "6/7 DB-Migration..."
progress "Datenbank wird migriert..." 80
"$PI/.venv/bin/python3" -c "
import sys; sys.path.insert(0, '$PI')
from backend.database import init_db
from backend.config_migration import ensure_config_defaults
init_db()
ensure_config_defaults()
print('  Migration abgeschlossen')
" 2>&1 | while read l; do log "$l"; done

# === 7. Services neu starten ===
log "7/7 Services neu starten..."
progress "Dienste werden neu gestartet..." 90
for svc in "${SERVICES[@]}"; do
    sudo systemctl enable "$svc" 2>/dev/null || true
done
# Das Backend zuletzt: Wird das Update über die Weboberfläche gestartet, läuft
# dieses Skript als Kindprozess des Backends (gleiche systemd-Control-Group)
# und wird beim Neustart des Backends mit beendet. Deshalb erst alle anderen
# Dienste neu starten und den Erfolg festhalten, dann das Backend ohne Warten
# (--no-block) neu starten.
for svc in "${SERVICES[@]}"; do
    [ "$svc" = "wage-pi-backend" ] && continue
    sudo systemctl restart "$svc" 2>/dev/null && log "  Restarted: $svc" || log "  Warnung: $svc konnte nicht gestartet werden"
done

log "=== Update abgeschlossen ==="
log "Commit: $(git log --oneline -1)"
set_update_state success "update erfolgreich" "Update abgeschlossen" "Update abgeschlossen" 100

log "  Restart (ohne Warten): wage-pi-backend"
trap - ERR
sudo systemctl restart --no-block wage-pi-backend 2>/dev/null || log "  Warnung: wage-pi-backend konnte nicht neu gestartet werden"
