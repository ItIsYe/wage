#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

sudo apt-get update
sudo apt-get install -y python3 python3-venv python3-pip sqlite3 i2c-tools jq curl
if ! command -v chromium-browser >/dev/null 2>&1 && ! command -v chromium >/dev/null 2>&1; then
  sudo apt-get install -y chromium-browser || sudo apt-get install -y chromium || true
fi

python3 -m venv "$ROOT/.venv"
source "$ROOT/.venv/bin/activate"
pip install --upgrade pip
pip install -r "$ROOT/requirements.txt"
mkdir -p "$ROOT/data" "$ROOT/logs"

bash "$ROOT/scripts/create_services.sh"
sudo systemctl enable wage-pi-backend wage-pi-oled wage-pi-leds wage-pi-kiosk wage-pi-screen-save wage-pi-hw-mgmt

# NetworkManager Dispatcher: Default-Route auf eth0 wenn LAN im AP-Modus
sudo install -m 0755 "$ROOT/scripts/nm_dispatcher_eth_route.sh" /etc/NetworkManager/dispatcher.d/99-wage-eth-route
echo "NM-Dispatcher installiert: /etc/NetworkManager/dispatcher.d/99-wage-eth-route"

# Cronjob: ESP-Firmware alle 5 Minuten automatisch syncen
chmod +x "$ROOT/scripts/firmware_sync_cron.sh"
CRON_JOB="*/5 * * * * $ROOT/scripts/firmware_sync_cron.sh >> $ROOT/logs/firmware_sync.log 2>&1"
( crontab -l 2>/dev/null | grep -v "firmware_sync_cron"; echo "$CRON_JOB" ) | crontab -
echo "Cronjob eingerichtet: ESP-Firmware-Sync alle 5 Minuten"

echo "Installation abgeschlossen"
echo "Start: sudo systemctl start wage-pi-backend wage-pi-oled wage-pi-leds"
echo "Web:  http://<pi-ip>:8000"

# shutdown ohne Passwort für wage User (Auto-Shutdown)
echo "wage ALL=(ALL) NOPASSWD: /sbin/shutdown" | sudo tee /etc/sudoers.d/wage-shutdown
sudo chmod 440 /etc/sudoers.d/wage-shutdown
echo 'SUBSYSTEM=="backlight", ACTION=="add", RUN+="/bin/chmod 666 /sys/class/backlight/%k/brightness"' | sudo tee /etc/udev/rules.d/99-backlight.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
