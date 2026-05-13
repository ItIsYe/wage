#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
sudo apt-get update
sudo apt-get install -y python3 python3-venv python3-pip sqlite3 i2c-tools chromium-browser
python3 -m venv "$ROOT/.venv"
source "$ROOT/.venv/bin/activate"
pip install --upgrade pip
pip install -r "$ROOT/requirements.txt"
mkdir -p "$ROOT/data" "$ROOT/logs"
bash "$ROOT/scripts/create_services.sh"
echo "Installation abgeschlossen. Start: sudo systemctl start wage-pi-backend"
