#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB_PATH="$ROOT_DIR/data/wage_pi.sqlite3"
LOG_FILE="$ROOT_DIR/logs/network_apply.log"

log(){ echo "[$(date -Iseconds)] $*" | tee -a "$LOG_FILE"; }
fail(){ log "FEHLER: $*"; exit 1; }

mkdir -p "$(dirname "$LOG_FILE")"

command -v nmcli >/dev/null 2>&1 || fail "nmcli fehlt"
command -v sqlite3 >/dev/null 2>&1 || fail "sqlite3 CLI fehlt"
[[ -f "$DB_PATH" ]] || fail "SQLite-Datei fehlt: $DB_PATH"

get_state() {
  local key="$1"
  sqlite3 -noheader "$DB_PATH" "SELECT COALESCE((SELECT value FROM app_state WHERE key='${key}' LIMIT 1),'');"
}

MODE="$(get_state network_mode)"; MODE="${MODE:-ap}"
AP_SSID="$(get_state ap_ssid)"; AP_SSID="${AP_SSID:-wage-net}"
AP_PASSWORD="$(get_state ap_password)"
AP_IP="$(get_state ap_ip)"; AP_IP="${AP_IP:-192.168.50.1}"
CLIENT_SSID="$(get_state client_ssid)"
CLIENT_PASSWORD="$(get_state client_password)"
AP_BAND="$(get_state ap_band)"; AP_BAND="${AP_BAND:-bg}"
AP_CHANNEL="$(get_state ap_channel)"; AP_CHANNEL="${AP_CHANNEL:-6}"
AP_PMF="$(get_state ap_pmf)"; AP_PMF="${AP_PMF:-0}"
AP_DHCP_START="$(get_state ap_dhcp_start)"
AP_DHCP_END="$(get_state ap_dhcp_end)"

# DHCP-Bereich des AP: NetworkManager startet im "shared"-Modus einen eigenen dnsmasq und
# liest zusaetzliche Optionen aus diesem Ordner. Fehlt die Datei, gilt der Standard-Bereich.
DHCP_CONF_DIR="${WAGE_DHCP_CONF_DIR:-/etc/NetworkManager/dnsmasq-shared.d}"
DHCP_CONF="$DHCP_CONF_DIR/wage-dhcp.conf"

ip2int() { local a b c d; IFS=. read -r a b c d <<<"$1"; echo $(( (10#$a << 24) + (10#$b << 16) + (10#$c << 8) + 10#$d )); }
valid_ipv4() {
  [[ "$1" =~ ^([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})$ ]] || return 1
  local o; for o in "${BASH_REMATCH[@]:1}"; do (( 10#$o <= 255 )) || return 1; done
}
# Prueft den Bereich wie das Backend (api_network_config.py): im /24 der AP-IP, Start <= Ende,
# AP-IP nicht im Bereich. Rueckgabe 0 = gueltig.
dhcp_range_valid() {
  valid_ipv4 "$AP_DHCP_START" && valid_ipv4 "$AP_DHCP_END" && valid_ipv4 "$AP_IP" || return 1
  local s e a; s="$(ip2int "$AP_DHCP_START")"; e="$(ip2int "$AP_DHCP_END")"; a="$(ip2int "$AP_IP")"
  (( (s >> 8) == (a >> 8) && (e >> 8) == (a >> 8) && s <= e && !(a >= s && a <= e) ))
}

# WLAN-Interfaces: eingebautes WLAN (AP fuer die Waage) und optional ein USB-WLAN-Stick
# (Verbindung ins Haus-WLAN im Modus ap_client). Die Namen wlan0/wlan1 koennen sich je nach
# Boot-Reihenfolge vertauschen, deshalb wird ueber den USB-Pfad unterschieden.
SYSFS_NET="${WAGE_SYSFS_NET:-/sys/class/net}"   # ueberschreibbar fuer Tests
is_usb_iface() { [[ "$(readlink -f "$SYSFS_NET/$1/device" 2>/dev/null)" == */usb* ]]; }
WIFI_IFACES=($(nmcli -t -f DEVICE,TYPE device status | awk -F: '$2=="wifi"{print $1}'))
[[ ${#WIFI_IFACES[@]} -gt 0 ]] || fail "Kein WLAN-Interface gefunden"
ONBOARD_IFACE=""; USB_IFACE=""
for i in "${WIFI_IFACES[@]}"; do
  if is_usb_iface "$i"; then [[ -n "$USB_IFACE" ]] || USB_IFACE="$i"
  else [[ -n "$ONBOARD_IFACE" ]] || ONBOARD_IFACE="$i"; fi
done
WLAN_IFACE="${ONBOARD_IFACE:-${WIFI_IFACES[0]}}"   # AP bzw. Client im Einzelmodus
log "WLAN-Interfaces: eingebaut=${ONBOARD_IFACE:-–} usb=${USB_IFACE:-–}"

CLIENT_CON="wage-net-client"

# Verbindung ins Haus-WLAN auf dem angegebenen Interface. Rueckgabe != 0 bei Fehler.
connect_client() {
  local iface="$1"
  log "Client: SSID=$CLIENT_SSID ifname=$iface passwort_gesetzt=$([[ -n "$CLIENT_PASSWORD" ]] && echo true || echo false)"
  nmcli device wifi rescan ifname "$iface" 2>/dev/null || true
  sleep 2
  # Alte Verbindungen fuer diese SSID loeschen, damit NM keine kaputte Config wiederverwendet
  nmcli connection delete "$CLIENT_CON" >/dev/null 2>&1 || true
  nmcli connection delete "$CLIENT_SSID" >/dev/null 2>&1 || true
  CONNECT_OUT=""; CONNECT_RC=0
  if [[ -n "$CLIENT_PASSWORD" ]]; then
    CONNECT_OUT=$(nmcli --wait 20 device wifi connect "$CLIENT_SSID" password "$CLIENT_PASSWORD" ifname "$iface" name "$CLIENT_CON" 2>&1) || CONNECT_RC=$?
  else
    CONNECT_OUT=$(nmcli --wait 20 device wifi connect "$CLIENT_SSID" ifname "$iface" name "$CLIENT_CON" 2>&1) || CONNECT_RC=$?
  fi
  [[ $CONNECT_RC -eq 0 ]] || return 1
  # Verbindung fest an dieses Interface binden, damit sie nach dem Booten nicht auf dem AP-WLAN landet
  nmcli connection modify "$CLIENT_CON" connection.interface-name "$iface" connection.autoconnect yes >/dev/null 2>&1 || true
  return 0
}

if [[ "$MODE" == "ap_client" ]]; then
  [[ -n "$USB_IFACE" ]] || fail "Modus AP + Haus-WLAN braucht einen USB-WLAN-Stick, es wurde keiner gefunden"
  [[ -n "$ONBOARD_IFACE" ]] || fail "Kein eingebautes WLAN fuer den AP gefunden"
  [[ -n "$CLIENT_SSID" ]] || fail "Client-SSID fehlt"
  WLAN_IFACE="$ONBOARD_IFACE"
fi

# Bestehende WLAN-Verbindungen sauber trennen bevor umgeschaltet wird
log "Trenne bestehende WLAN-Verbindungen..."
nmcli connection down wage-net-ap >/dev/null 2>&1 || true
nmcli device disconnect "$WLAN_IFACE" >/dev/null 2>&1 || true
if [[ -n "$USB_IFACE" ]]; then
  nmcli device disconnect "$USB_IFACE" >/dev/null 2>&1 || true
fi
if [[ "$MODE" == "ap" ]]; then
  # Reiner AP-Modus: eine Haus-WLAN-Verbindung des Sticks darf nach dem Booten nicht wieder hochkommen
  nmcli connection delete "$CLIENT_CON" >/dev/null 2>&1 || true
fi
sleep 1

# AP fuer die Waage auf $WLAN_IFACE starten (auch als Rueckfall, wenn das Haus-WLAN im Client-Modus
# scheitert). Rueckgabe != 0 bei Fehler, Grund in AP_ERR.
start_ap() {
  AP_ERR=""
  [[ -n "$AP_SSID" ]] || { AP_ERR="AP-SSID darf nicht leer sein"; return 1; }
  [[ -n "$AP_IP" ]] || { AP_ERR="AP-IP darf nicht leer sein"; return 1; }

  [[ "$AP_BAND" == "bg" ]] || AP_BAND="bg"
  [[ "$AP_CHANNEL" =~ ^[0-9]+$ ]] || AP_CHANNEL="6"
  [[ "$AP_PMF" == "0" || "$AP_PMF" == "1" ]] || AP_PMF="0"

  if [[ -z "$AP_PASSWORD" ]]; then
    AP_ERR="AP-Passwort fehlt. Bitte im Webinterface ein AP-Passwort mit mindestens 8 Zeichen setzen."; return 1
  elif [[ ${#AP_PASSWORD} -lt 8 ]]; then
    AP_ERR="AP-Passwort zu kurz (mindestens 8 Zeichen erforderlich)"; return 1
  fi

  DHCP_NOTE=""
  if [[ -n "$AP_DHCP_START" || -n "$AP_DHCP_END" ]] && dhcp_range_valid; then
    mkdir -p "$DHCP_CONF_DIR"
    printf 'dhcp-range=%s,%s,255.255.255.0,12h\n' "$AP_DHCP_START" "$AP_DHCP_END" > "$DHCP_CONF"
    log "DHCP-Bereich: $AP_DHCP_START - $AP_DHCP_END"
  else
    rm -f "$DHCP_CONF"
    log "DHCP-Bereich ungueltig oder leer - NetworkManager-Standard wird verwendet"
  fi

  nmcli connection delete wage-net-ap >/dev/null 2>&1 || true
  nmcli connection add type wifi ifname "$WLAN_IFACE" con-name wage-net-ap autoconnect yes ssid "$AP_SSID" >/dev/null \
    || { AP_ERR="AP-Profil konnte nicht angelegt werden"; return 1; }
  nmcli connection modify wage-net-ap \
    802-11-wireless.mode ap \
    802-11-wireless.band "$AP_BAND" \
    802-11-wireless.channel "$AP_CHANNEL" \
    ipv4.method shared \
    ipv4.addresses "$AP_IP/24" \
    802-11-wireless-security.key-mgmt wpa-psk \
    802-11-wireless-security.proto rsn \
    802-11-wireless-security.pairwise ccmp \
    802-11-wireless-security.group ccmp \
    802-11-wireless-security.pmf "$AP_PMF" \
    802-11-wireless-security.psk "$AP_PASSWORD" \
    || { AP_ERR="AP-Profil konnte nicht eingerichtet werden"; return 1; }

  if ! nmcli connection up wage-net-ap >/dev/null 2>&1; then
    sleep 2
    if ! nmcli connection up wage-net-ap >/dev/null 2>&1; then
      if [[ -f "$DHCP_CONF" ]]; then
        # Der eigene DHCP-Bereich koennte den DHCP-Server am Start hindern: entfernen und
        # mit dem Standard-Bereich erneut versuchen, damit der AP auf jeden Fall erreichbar bleibt.
        log "AP startet nicht - entferne eigenen DHCP-Bereich und versuche es mit dem Standard erneut"
        rm -f "$DHCP_CONF"
        DHCP_NOTE="WARNUNG: DHCP-Bereich konnte nicht angewendet werden, Standard-Bereich aktiv"
        nmcli connection up wage-net-ap >/dev/null || { AP_ERR="AP konnte nicht gestartet werden"; return 1; }
      else
        AP_ERR="AP konnte nicht gestartet werden"; return 1
      fi
    fi
  fi
  log "AP-Modus erfolgreich aktiviert"
  [[ -z "$DHCP_NOTE" ]] || log "$DHCP_NOTE"
  log "Security=WPA2-PSK/RSN/CCMP PMF=$AP_PMF Band=$AP_BAND Channel=$AP_CHANNEL"

  # Default-Route auf eth0 setzen wenn LAN verbunden
  ETH_IFACE="$(nmcli -t -f DEVICE,TYPE device status | awk -F: '$2=="ethernet"{print $1; exit}')"
  if [[ -n "$ETH_IFACE" ]]; then
    ETH_GW="$(ip route show dev "$ETH_IFACE" 2>/dev/null | awk '/default/{print $3; exit}')"
    if [[ -n "$ETH_GW" ]]; then
      ip route del default dev "$WLAN_IFACE" 2>/dev/null || true
      ip route replace default via "$ETH_GW" dev "$ETH_IFACE" metric 100
      log "LAN erkannt ($ETH_IFACE gw=$ETH_GW): Default-Route auf eth0 gesetzt"
    else
      log "LAN-Interface $ETH_IFACE gefunden aber kein Gateway"
    fi
  elif [[ "$MODE" != "ap_client" ]]; then
    log "Kein LAN verbunden — kein Internet im AP-Modus (normal)"
  fi
  return 0
}

if [[ "$MODE" == "ap" || "$MODE" == "ap_client" ]]; then
  # Ein Client-Profil aus aelteren Versionen (benannt nach der SSID) darf nach einem Neustart nicht das
  # AP-Profil verdraengen
  if [[ -n "$CLIENT_SSID" ]]; then
    nmcli connection modify "$CLIENT_SSID" connection.autoconnect no >/dev/null 2>&1 || true
  fi
  log "Modus=$MODE SSID=$AP_SSID IP=$AP_IP ifname=$WLAN_IFACE passwort_gesetzt=$([[ -n "$AP_PASSWORD" ]] && echo true || echo false)"
  start_ap || fail "$AP_ERR"

  if [[ "$MODE" == "ap_client" ]]; then
    # Der AP laeuft bereits; scheitert der Stick, bleibt die Waage trotzdem erreichbar.
    if connect_client "$USB_IFACE"; then
      log "Haus-WLAN ueber USB-Stick ($USB_IFACE) verbunden: $CONNECT_OUT"
      log "Modus AP + Haus-WLAN erfolgreich aktiviert"
    else
      fail "AP aktiv, aber Haus-WLAN ueber USB-Stick fehlgeschlagen (rc=$CONNECT_RC): $CONNECT_OUT"
    fi
  fi
else
  log "Modus=client"
  [[ -n "$CLIENT_SSID" ]] || fail "Client-SSID fehlt"
  if ! connect_client "$WLAN_IFACE"; then
    # Ohne Rueckfall waere der AP abgebaut: die Waage verliert den Pi, der Pi ist nur noch per LAN erreichbar.
    CLIENT_ERR="WLAN-Verbindung fehlgeschlagen (rc=$CONNECT_RC): $CONNECT_OUT"
    log "$CLIENT_ERR"
    log "Rueckfall: starte den AP fuer die Waage wieder..."
    # Halb angelegtes Client-Profil entfernen, sonst konkurriert es nach einem Neustart mit dem AP
    nmcli connection delete "$CLIENT_CON" >/dev/null 2>&1 || true
    if start_ap; then
      # Gespeicherten Modus an den tatsaechlichen Zustand anpassen (SSID/Passwort bleiben gespeichert)
      sqlite3 "$DB_PATH" "UPDATE app_state SET value='ap' WHERE key='network_mode';" 2>/dev/null || true
      fail "$CLIENT_ERR – AP wurde wiederhergestellt, Modus auf AP zurueckgesetzt"
    else
      fail "$CLIENT_ERR – AP-Rueckfall ebenfalls fehlgeschlagen: $AP_ERR"
    fi
  fi
  # Das AP-Profil (autoconnect yes) koennte sonst nach einem Neustart wieder statt des Haus-WLANs starten
  nmcli connection modify wage-net-ap connection.autoconnect no >/dev/null 2>&1 || true
  log "Client-Modus erfolgreich aktiviert: $CONNECT_OUT"
fi
