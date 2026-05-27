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
AP_PMF="$(get_state ap_pmf)"; AP_PMF="${AP_PMF:-1}"

WLAN_IFACE="$(nmcli -t -f DEVICE,TYPE device status | awk -F: '$2=="wifi"{print $1; exit}')"
[[ -n "$WLAN_IFACE" ]] || fail "Kein WLAN-Interface gefunden"

if [[ "$MODE" == "ap" ]]; then
  log "Modus=ap SSID=$AP_SSID IP=$AP_IP passwort_gesetzt=$([[ -n "$AP_PASSWORD" ]] && echo true || echo false)"
  [[ -n "$AP_SSID" ]] || fail "AP-SSID darf nicht leer sein"
  [[ -n "$AP_IP" ]] || fail "AP-IP darf nicht leer sein"

  [[ "$AP_BAND" == "bg" ]] || AP_BAND="bg"
  [[ "$AP_CHANNEL" =~ ^[0-9]+$ ]] || AP_CHANNEL="6"
  [[ "$AP_CHANNEL" == "6" ]] || AP_CHANNEL="6"
  [[ "$AP_PMF" == "0" || "$AP_PMF" == "1" ]] || AP_PMF="1"

  if [[ -z "$AP_PASSWORD" ]]; then
    fail "AP-Passwort fehlt. Bitte im Webinterface ein AP-Passwort mit mindestens 8 Zeichen setzen."
  elif [[ ${#AP_PASSWORD} -lt 8 ]]; then
    fail "AP-Passwort zu kurz (mindestens 8 Zeichen erforderlich)"
  fi

  nmcli connection delete wage-net-ap >/dev/null 2>&1 || true
  nmcli connection add type wifi ifname "$WLAN_IFACE" con-name wage-net-ap autoconnect yes ssid "$AP_SSID" >/dev/null
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
    802-11-wireless-security.psk "$AP_PASSWORD"

  nmcli connection up wage-net-ap >/dev/null
  log "AP-Modus erfolgreich aktiviert"
  log "Security=WPA2-PSK/RSN/CCMP PMF=optional Band=bg Channel=6"
else
  log "Modus=client SSID=$CLIENT_SSID passwort_gesetzt=$([[ -n "$CLIENT_PASSWORD" ]] && echo true || echo false)"
  [[ -n "$CLIENT_SSID" ]] || fail "Client-SSID fehlt"

  nmcli connection down wage-net-ap >/dev/null 2>&1 || true
  if [[ -n "$CLIENT_PASSWORD" ]]; then
    nmcli device wifi connect "$CLIENT_SSID" password "$CLIENT_PASSWORD" ifname "$WLAN_IFACE" >/dev/null
  else
    nmcli device wifi connect "$CLIENT_SSID" ifname "$WLAN_IFACE" >/dev/null
  fi
  log "Client-Modus erfolgreich aktiviert"
fi
