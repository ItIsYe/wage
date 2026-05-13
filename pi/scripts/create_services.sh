#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
sudo cp "$ROOT"/systemd/wage-pi-*.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable wage-pi-backend wage-pi-oled wage-pi-leds
