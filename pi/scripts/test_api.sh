#!/usr/bin/env bash
set -euo pipefail
BASE="${1:-http://localhost:8000}"
SUFFIX="$(date +%s)"

curl -sf "$BASE/api/v1/health" | jq .ok
PID=$(curl -sf -X POST "$BASE/api/v1/persons" -H 'content-type: application/json' -d "{\"name\":\"Testperson-$SUFFIX\",\"activate\":true}" | jq -r .person_id)
curl -sf -X POST "$BASE/api/v1/persons/$PID/activate" | jq .active_person_id
PAY="{\"protocol_version\":\"1.0\",\"device_id\":\"scale-001\",\"boot_id\":\"boot-$SUFFIX\",\"run_number\":1,\"event_id\":\"evt-$SUFFIX\",\"time_ms\":12345,\"start_weight_g\":87.5,\"status\":\"ok\",\"firmware_version\":\"0.1.0\",\"queue_depth\":0}"
curl -sf -X POST "$BASE/api/v1/runs" -H 'content-type: application/json' -d "$PAY" | jq .accepted
curl -sf -X POST "$BASE/api/v1/runs" -H 'content-type: application/json' -d "$PAY" | jq .duplicate
curl -sf "$BASE/api/v1/runs?limit=5" | jq .count
