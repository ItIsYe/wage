#!/usr/bin/env bash
# /etc/NetworkManager/dispatcher.d/99-wage-eth-route
# Wird von NetworkManager aufgerufen wenn ein Interface hoch/runtergeht.
# Sorgt dafür dass im AP-Modus Internet über eth0 läuft wenn LAN eingesteckt wird.

IFACE="$1"
ACTION="$2"

# Nur bei Ethernet-Events relevant
[[ "$IFACE" == eth* ]] || exit 0
[[ "$ACTION" == "up" || "$ACTION" == "down" ]] || exit 0

LOG_FILE="/home/wage/wage/pi/logs/network_apply.log"
log() { echo "[$(date -Iseconds)] [dispatcher] $*" >> "$LOG_FILE" 2>/dev/null || true; }

# Prüfen ob wage-net AP aktiv ist
WLAN_IFACE="$(nmcli -t -f DEVICE,TYPE device status 2>/dev/null | awk -F: '$2=="wifi"{print $1; exit}')"
AP_ACTIVE="$(nmcli -t -f NAME,DEVICE connection show --active 2>/dev/null | grep wage-net-ap | head -1)"

if [[ -z "$AP_ACTIVE" ]]; then
  # AP nicht aktiv, nichts zu tun
  exit 0
fi

if [[ "$ACTION" == "up" ]]; then
  sleep 2  # kurz warten bis DHCP-Lease da ist
  ETH_GW="$(ip route show dev "$IFACE" | awk '/default/{print $3; exit}')"
  if [[ -n "$ETH_GW" ]]; then
    # wlan Default-Route entfernen, eth0 Default-Route setzen
    ip route del default dev "$WLAN_IFACE" 2>/dev/null || true
    ip route replace default via "$ETH_GW" dev "$IFACE" metric 100
    log "LAN $IFACE up (gw=$ETH_GW): Default-Route auf $IFACE gesetzt"
  else
    log "LAN $IFACE up aber kein Gateway via DHCP"
  fi

elif [[ "$ACTION" == "down" ]]; then
  # eth0 weg — Default-Route wieder auf wlan falls AP-Gateway existiert
  AP_GW="$(ip route show dev "$WLAN_IFACE" | awk '/src/{print $1; exit}')"
  ip route del default dev "$IFACE" 2>/dev/null || true
  log "LAN $IFACE down: Default-Route entfernt"
fi
