#!/usr/bin/env bash
set -euo pipefail
URL="${WAGE_PI_KIOSK_URL:-http://localhost:8000}"
BROWSER="$(command -v chromium-browser || command -v chromium || true)"
if [ -z "$BROWSER" ]; then
  echo "Kein Chromium gefunden" >&2
  exit 1
fi

exec "$BROWSER"   --kiosk   --incognito   --noerrdialogs   --disable-infobars   --touch-events=enabled   --enable-features=VirtualKeyboard   --disable-features=TranslateUI   "$URL"
