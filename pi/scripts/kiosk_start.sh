#!/usr/bin/env bash
set -euo pipefail
URL="${WAGE_PI_KIOSK_URL:-http://localhost:8000}"
BROWSER="$(command -v chromium-browser || command -v chromium || true)"
if [ -z "$BROWSER" ]; then
  echo "Kein Chromium gefunden" >&2
  exit 1
fi
"$BROWSER" --kiosk --incognito --disable-translate --noerrdialogs --disable-infobars "$URL"
