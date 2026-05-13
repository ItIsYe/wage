#!/usr/bin/env bash
set -euo pipefail
URL="http://localhost:8000"
chromium-browser --kiosk --incognito --disable-infobars "$URL"
