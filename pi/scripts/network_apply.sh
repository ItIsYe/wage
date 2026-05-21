#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CFG_JSON="$ROOT_DIR/data/network_config.json"
LOG_FILE="$ROOT_DIR/logs/network_apply.log"
log(){ echo "[$(date -Iseconds)] $*" | tee -a "$LOG_FILE"; }
if ! command -v nmcli >/dev/null 2>&1; then log "nmcli fehlt"; exit 10; fi
if [[ ! -f "$CFG_JSON" ]]; then log "Konfigurationsdatei fehlt: $CFG_JSON"; exit 11; fi
MODE="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1])).get("network_mode","ap"))' "$CFG_JSON")"
WLAN_IFACE="$(nmcli -t -f DEVICE,TYPE device status | awk -F: '$2=="wifi"{print $1; exit}')"
if [[ -z "$WLAN_IFACE" ]]; then log "Kein WLAN-Interface gefunden"; exit 12; fi
if [[ "$MODE" == "ap" ]]; then
  SSID="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1])).get("ap_ssid","wage-net"))' "$CFG_JSON")"
  log "Aktiviere AP-Modus: $SSID"
  nmcli connection delete wage-net-ap >/dev/null 2>&1 || true
  nmcli connection add type wifi ifname "$WLAN_IFACE" con-name wage-net-ap autoconnect yes ssid "$SSID" >/dev/null
  nmcli connection modify wage-net-ap 802-11-wireless.mode ap ipv4.method shared ipv4.addresses "192.168.50.1/24"
  nmcli connection up wage-net-ap >/dev/null
  log "AP-Modus aktiv"
else
  SSID="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1])).get("client_ssid",""))' "$CFG_JSON")"
  if [[ -z "$SSID" ]]; then log "Client SSID fehlt"; exit 13; fi
  log "Aktiviere Client-Modus: $SSID"
  nmcli connection up "$SSID" >/dev/null 2>&1 || nmcli device wifi connect "$SSID" ifname "$WLAN_IFACE" >/dev/null
  nmcli connection down wage-net-ap >/dev/null 2>&1 || true
  log "Client-Modus aktiv"
fi
exit 0
