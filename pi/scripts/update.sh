#!/usr/bin/env bash
# /home/wage/wage/pi/scripts/update.sh
# Vollständiger Update-Prozess für wage-pi
# Wird vom Pi-Webinterface aufgerufen oder manuell: bash pi/scripts/update.sh

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PI="$ROOT/pi"
LOG="$PI/logs/update.log"
BACKUP_DIR="$PI/data/backups"
DB="$PI/data/wage_pi.sqlite3"

mkdir -p "$PI/logs" "$BACKUP_DIR"

log() { echo "[$(date -Iseconds)] $*" | tee -a "$LOG"; }
fail() { log "FEHLER: $*"; exit 1; }

log "=== wage-pi Update gestartet ==="

# === 1. DB-Backup ===
log "1/7 DB-Backup..."
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
cd "$ROOT"
git fetch origin beta 2>&1 | while read l; do log "  git: $l"; done
git reset --hard origin/beta 2>&1 | while read l; do log "  git: $l"; done
sudo chown -R wage:wage "$ROOT"
log "  Code aktuell: $(git log --oneline -1)"

# === 3. Python-Pakete ===
log "3/7 Python-Pakete aktualisieren..."
"$PI/.venv/bin/pip" install -q -r "$PI/requirements.txt" --upgrade 2>&1 | tail -5 | while read l; do log "  pip: $l"; done
log "  Pakete aktuell"

# === 4. Systemd Services installieren ===
log "4/7 Systemd Services aktualisieren..."
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
if [ -f "$PI/scripts/nm_dispatcher_eth_route.sh" ]; then
    sudo install -m 0755 "$PI/scripts/nm_dispatcher_eth_route.sh" /etc/NetworkManager/dispatcher.d/99-wage-eth-route
    log "  NM-Dispatcher aktualisiert"
fi

# === 6. DB-Migration und Config-Defaults ===
log "6/7 DB-Migration..."
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
for svc in "${SERVICES[@]}"; do
    sudo systemctl enable "$svc" 2>/dev/null || true
    sudo systemctl restart "$svc" 2>/dev/null && log "  Restarted: $svc" || log "  Warnung: $svc konnte nicht gestartet werden"
done

log "=== Update abgeschlossen ==="
log "Commit: $(git log --oneline -1)"
