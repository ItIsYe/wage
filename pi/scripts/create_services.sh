#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
sudo install -m 0644 "$ROOT"/systemd/wage-pi-*.service /etc/systemd/system/
sudo systemctl daemon-reload
echo "Service-Dateien installiert."
