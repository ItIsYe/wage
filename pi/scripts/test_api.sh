#!/usr/bin/env bash
set -euo pipefail
BASE="${WAGE_PI_URL:-http://localhost:8000}"
SUFFIX="$(date +%s)"

ok(){ echo "✅ $1"; }
fail(){ echo "❌ $1" >&2; exit 1; }

echo "Teste API unter: $BASE"

curl -sf "$BASE/api/v1/health" >/dev/null || fail "Healthcheck fehlgeschlagen"
ok "Healthcheck erreichbar"

PERSON_PAYLOAD='{"name":"Testperson","activate":false}'
PERSON_RESP=$(curl -s -o /tmp/wage_person.json -w '%{http_code}' -X POST "$BASE/api/v1/persons" -H 'content-type: application/json' -d "$PERSON_PAYLOAD")
if [ "$PERSON_RESP" = "409" ]; then
  PERSON_PAYLOAD="{\"name\":\"Testperson-$SUFFIX\",\"activate\":false}"
  curl -sf -X POST "$BASE/api/v1/persons" -H 'content-type: application/json' -d "$PERSON_PAYLOAD" > /tmp/wage_person.json || fail "Person anlegen fehlgeschlagen"
else
  [ "$PERSON_RESP" = "200" ] || fail "Person anlegen fehlgeschlagen (HTTP $PERSON_RESP)"
fi
PID=$(jq -r '.person_id' /tmp/wage_person.json)
[ -n "$PID" ] && [ "$PID" != "null" ] || fail "Keine person_id erhalten"
ok "Person angelegt (ID $PID)"

curl -sf -X POST "$BASE/api/v1/persons/$PID/activate" >/dev/null || fail "Aktivieren fehlgeschlagen"
ok "Testperson aktiv gesetzt"

PAY="{\"protocol_version\":\"1.0\",\"device_id\":\"scale-001\",\"boot_id\":\"boot-$SUFFIX\",\"run_number\":1,\"event_id\":\"evt-$SUFFIX\",\"time_ms\":12345,\"start_weight_g\":87.5,\"status\":\"ok\",\"firmware_version\":\"0.1.0\",\"queue_depth\":0}"
RESP1=$(curl -sf -X POST "$BASE/api/v1/runs" -H 'content-type: application/json' -d "$PAY")
echo "$RESP1" | jq -e '.accepted == true and .duplicate == false' >/dev/null || fail "Erster Lauf wurde nicht korrekt akzeptiert"
ok "Testlauf gespeichert"

RESP2=$(curl -sf -X POST "$BASE/api/v1/runs" -H 'content-type: application/json' -d "$PAY")
echo "$RESP2" | jq -e '.accepted == true and .duplicate == true' >/dev/null || fail "Duplikat wurde nicht erkannt"
ok "Duplikat korrekt erkannt"

curl -sf "$BASE/api/v1/runs?limit=5" | jq -e '.count >= 1' >/dev/null || fail "Läufe konnten nicht geladen werden"
ok "Läufe abrufbar"

curl -sf "$BASE/api/v1/status" | jq -e '.led_status != null and .oled_status != null' >/dev/null || fail "Status unvollständig"
ok "Status abrufbar (inkl. LED/OLED)"

echo "Alle Tests erfolgreich abgeschlossen."
