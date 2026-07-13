#!/usr/bin/env bash
# Wird per Cronjob alle 5 Minuten ausgeführt.
# Prüft ob eine neue ESP-Firmware auf GitHub verfügbar ist und lädt sie automatisch.

set -euo pipefail

API_BASE="http://localhost:8000"
LOG_FILE="$(cd "$(dirname "$0")/.." && pwd)/logs/firmware_sync.log"
mkdir -p "$(dirname "$LOG_FILE")"

log() { echo "[$(date -Iseconds)] $*" >> "$LOG_FILE"; }

# Internet prüfen
if ! ip route show default | grep -q "default"; then
  log "Kein Internet — überspringe Sync"
  exit 0
fi

# Aktuelles gecachtes Manifest lesen
CACHED_VERSION=""
CACHED=$(curl -sf --max-time 5 "$API_BASE/api/v1/esp-firmware/manifest" 2>/dev/null || true)
if [[ -n "$CACHED" ]]; then
  CACHED_VERSION=$(echo "$CACHED" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('version',''))" 2>/dev/null || true)
fi

# Neuestes Manifest direkt von GitHub lesen
LATEST=$(curl -sf --max-time 10 \
  -H "Accept: application/vnd.github+json" \
  "https://api.github.com/repos/ItIsYe/wage/releases/tags/esp32-latest" 2>/dev/null || true)

if [[ -z "$LATEST" ]]; then
  log "GitHub nicht erreichbar — überspringe Sync"
  exit 0
fi

# Asset-URL für manifest.json aus Release holen
MANIFEST_URL=$(echo "$LATEST" | python3 -c "
import sys, json
d = json.load(sys.stdin)
for a in d.get('assets', []):
    if a['name'] == 'manifest.json':
        print(a['browser_download_url'])
        break
" 2>/dev/null || true)

if [[ -z "$MANIFEST_URL" ]]; then
  log "Kein manifest.json im Release — überspringe Sync"
  exit 0
fi

LATEST_VERSION=$(curl -sf --max-time 10 "$MANIFEST_URL" 2>/dev/null | \
  python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('version',''))" 2>/dev/null || true)

if [[ -z "$LATEST_VERSION" ]]; then
  log "Versions-Abruf fehlgeschlagen"
  exit 0
fi

if [[ "$CACHED_VERSION" == "$LATEST_VERSION" ]]; then
  log "Bereits aktuell ($LATEST_VERSION) — kein Sync nötig"
  exit 0
fi

log "Neue Version gefunden: $CACHED_VERSION -> $LATEST_VERSION — starte Sync..."
RESULT=$(curl -sf --max-time 60 -X POST "$API_BASE/api/v1/esp-firmware/sync" 2>/dev/null || true)
if [[ -n "$RESULT" ]]; then
  log "Sync erfolgreich: $LATEST_VERSION"
else
  log "Sync fehlgeschlagen"
fi
