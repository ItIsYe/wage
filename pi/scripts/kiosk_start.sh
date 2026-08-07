#!/usr/bin/env bash
set -euo pipefail
export DISPLAY="${DISPLAY:-:0}"
export XAUTHORITY="${XAUTHORITY:-/home/wage/.Xauthority}"
URL="${WAGE_PI_KIOSK_URL:-http://localhost:8000}"
BROWSER="$(command -v chromium-browser || command -v chromium || true)"
if [ -z "$BROWSER" ]; then
  echo "Kein Chromium gefunden" >&2
  exit 1
fi

# Alte Kiosk-Session löschen damit keine "existing browser session" entsteht
rm -rf /tmp/wage-kiosk
# Eventuell noch laufende Chromium-Prozesse beenden
pkill -f "user-data-dir=/tmp/wage-kiosk" 2>/dev/null || true
sleep 1

# Warten bis Backend erreichbar ist (max. 60s)
echo "Warte auf Backend unter $URL ..."
for i in $(seq 1 60); do
  if curl -sf --max-time 2 "$URL/api/v1/health" > /dev/null 2>&1; then
    echo "Backend bereit nach ${i}s"
    break
  fi
  sleep 1
done

exec "$BROWSER" \
  --kiosk \
  --incognito \
  --noerrdialogs \
  --disable-infobars \
  --touch-events=enabled \
  --enable-features=VirtualKeyboard \
  --disable-features=TranslateUI \
  --user-data-dir=/tmp/wage-kiosk \
  --disable-dev-shm-usage \
  --no-sandbox \
  --disable-gpu-sandbox \
  --disable-software-rasterizer \
  --disable-background-networking \
  --disable-default-apps \
  --disable-extensions \
  --disable-sync \
  --disable-translate \
  --disable-background-timer-throttling \
  --disable-renderer-backgrounding \
  --disable-backgrounding-occluded-windows \
  --memory-pressure-off \
  --max-old-space-size=128 \
  --app="$URL"
