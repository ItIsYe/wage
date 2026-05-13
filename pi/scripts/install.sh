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
sudo systemctl enable wage-pi-backend wage-pi-oled wage-pi-leds

echo "Installation abgeschlossen"
echo "Start: sudo systemctl start wage-pi-backend wage-pi-oled wage-pi-leds"
echo "Web:  http://<pi-ip>:8000"
