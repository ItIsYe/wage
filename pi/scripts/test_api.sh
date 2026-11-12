#!/usr/bin/env bash
set -euo pipefail
BASE="${WAGE_PI_URL:-http://localhost:8000}"
SUFFIX="$(date +%s)"

ok(){ echo "✅ $1"; }
fail(){ echo "❌ $1" >&2; exit 1; }

echo "Teste API unter: $BASE"
echo "Hinweis: legt kurz eine Testperson und einen Testlauf an und entfernt beide am Ende wieder."

curl -sf "$BASE/api/v1/health" >/dev/null || fail "Healthcheck fehlgeschlagen"
ok "Healthcheck erreichbar"

PREV_ACTIVE=$(curl -sf "$BASE/api/v1/persons" | jq -r '.active_person_id // 1')
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

PAY="{\"protocol_version\":1,\"device_id\":\"scale-001\",\"boot_id\":$SUFFIX,\"run_number\":1,\"event_id\":\"evt-$SUFFIX\",\"time_ms\":12345,\"start_weight_g\":87.5,\"status\":\"ok\",\"firmware_version\":\"0.1.0\",\"queue_depth\":0}"
RESP1=$(curl -sf -X POST "$BASE/api/v1/runs" -H 'content-type: application/json' -d "$PAY")
echo "$RESP1" | jq -e '.accepted == true and .duplicate == false' >/dev/null || fail "Erster Lauf wurde nicht korrekt akzeptiert"
RUN_ID=$(echo "$RESP1" | jq -r '.run_db_id')
ok "Testlauf gespeichert"

RESP2=$(curl -sf -X POST "$BASE/api/v1/runs" -H 'content-type: application/json' -d "$PAY")
echo "$RESP2" | jq -e '.accepted == true and .duplicate == true' >/dev/null || fail "Duplikat wurde nicht erkannt"
ok "Duplikat korrekt erkannt"

curl -sf "$BASE/api/v1/runs?limit=5" | jq -e '.count >= 1' >/dev/null || fail "Läufe konnten nicht geladen werden"
ok "Läufe abrufbar"

curl -sf "$BASE/api/v1/status" | jq -e '.led_status != null and .oled_status != null' >/dev/null || fail "Status unvollständig"
ok "Status abrufbar (inkl. LED/OLED)"

# Aufraeumen: Testlauf loeschen, vorher aktive Person wieder aktivieren, Testperson entfernen
curl -sf -X DELETE "$BASE/api/v1/runs/$RUN_ID" >/dev/null || true
curl -sf -X POST "$BASE/api/v1/persons/$PREV_ACTIVE/activate" >/dev/null || true
curl -sf -X DELETE "$BASE/api/v1/persons/$PID" >/dev/null || true

echo "Alle Tests erfolgreich abgeschlossen."
