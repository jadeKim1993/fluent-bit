#!/usr/bin/env bash
# End-to-end test of the out_http batching features in a Linux container.
#
#   bash documentation/examples/out_http_batch_e2e/e2e.sh
#
# Requires docker with the images:
#   fb-builder-pg  Ubuntu with the fluent-bit build tools and libpq-dev
#   postgres:16
set -u
KIT="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$KIT/../../.." && pwd)"
WORK="${WORK:-$(mktemp -d)}"
IMAGE="${IMAGE:-fb-builder-pg:latest}"
cp "$KIT"/recv.pl "$KIT"/show.pl "$KIT"/run.sh "$KIT"/t1.yaml "$KIT"/t2.yaml "$KIT"/t3.yaml "$WORK"/

PASS=0; FAIL=0
check() { if eval "$2"; then echo "    [PASS] $1"; PASS=$((PASS+1)); else echo "    [FAIL] $1"; FAIL=$((FAIL+1)); fi; }
q() { docker exec e2e-pg psql -U fluent -d logs -At -c "$1"; }

echo "=================================================================="
echo " out_http 배치 기능 E2E 테스트"
echo "=================================================================="
echo "  실행 시각 : $(date '+%Y-%m-%d %H:%M:%S %Z')"
echo "  소스      : $(git -C "$SRC" log --oneline -1)"
echo "  실행 환경 : docker $(docker version --format '{{.Server.Version}} ({{.Server.Os}}/{{.Server.Arch}})'), 이미지 $IMAGE"
echo "  DB        : postgres:16 컨테이너 (e2e-pg)"

docker network inspect e2e-net >/dev/null 2>&1 || docker network create e2e-net >/dev/null
docker rm -f e2e-pg >/dev/null 2>&1
docker run -d --name e2e-pg --network e2e-net -e POSTGRES_USER=fluent \
       -e POSTGRES_PASSWORD=fluent_test_pw -e POSTGRES_DB=logs postgres:16 >/dev/null
for i in $(seq 1 30); do docker exec e2e-pg pg_isready -U fluent -d logs >/dev/null 2>&1 && break; sleep 1; done

docker run --rm --name fb-e2e --network e2e-net -v "$SRC":/src:ro -v fb-linux-build:/build \
       -v "$WORK":/work "$IMAGE" bash /work/run.sh

echo "  ▶ PostgreSQL 의 batch_events 테이블 (호스트에서 조회)"
docker exec e2e-pg psql -U fluent -d logs -P pager=off -c "SELECT to_char(time,'HH24:MI:SS') AS time, data->>'status' AS status, data->>'http_status' AS http_status, data->>'records' AS records, data->>'carried_over_records' AS carried, data->>'attempts' AS attempts, data->>'duration_ms' AS ms, data->>'batch_id' AS batch_id, data->>'error' AS error FROM batch_events ORDER BY time;" | sed 's/^/    /'
echo "  ▶ 한 행의 data (jsonb) 전체"
docker exec e2e-pg psql -U fluent -d logs -At -c "SELECT jsonb_pretty(data) FROM batch_events WHERE data->>'status'='success' ORDER BY time LIMIT 1;" | sed 's/^/    /'
echo "  ▶ 판정"
SEQ=$(q "SELECT string_agg(data->>'status', ',' ORDER BY time) FROM batch_events")
S1=$(q "SELECT concat_ws('|', data->>'records', data->>'attempts', data->>'http_status', coalesce(data->>'error','null')) FROM batch_events WHERE data->>'status'='success' ORDER BY time LIMIT 1")
E1=$(q "SELECT concat_ws('|', data->>'records', data->>'attempts', coalesce(data->>'http_status','null')) FROM batch_events WHERE data->>'status'='empty' ORDER BY time LIMIT 1")
R1=$(q "SELECT concat_ws('|', data->>'records', data->>'attempts', coalesce(data->>'http_status','null'), data->>'error') FROM batch_events WHERE data->>'status'='retry_next_interval' ORDER BY time LIMIT 1")
S2=$(q "SELECT concat_ws('|', data->>'records', data->>'carried_over_records', data->>'http_status') FROM batch_events WHERE data->>'status'='success' ORDER BY time OFFSET 1 LIMIT 1")
DB_IDS=$(q "SELECT string_agg(data->>'batch_id', ',' ORDER BY time) FROM batch_events WHERE data->>'status'='success'")
HDR_IDS=$(cut -f5 "$WORK/t3_recv.log.tsv" | paste -sd, -)
check "이벤트 순서: success → empty → retry_next_interval → success ($SEQ)" '[[ "$SEQ" =~ ^success,empty,retry_next_interval(,retry_next_interval)*,success ]]'
check "첫 success: records=3, attempts=1, http_status=200, error=null ($S1)" '[ "$S1" = "3|1|200|null" ]'
check "empty: records=0, attempts=0, http_status=null ($E1)" '[ "$E1" = "0|0|null" ]'
check "retry_next_interval: records=2, attempts=2(즉시 재시도 포함), http_status=null, error 기록 ($R1)" '[[ "$R1" == 2\|2\|null\|?* ]]'
check "복구 후 success: records=2, carried_over_records=2, http_status=200 ($S2)" '[ "$S2" = "2|2|200" ]'
check "요청의 X-Batch-Id 헤더 = DB 의 success batch_id ($HDR_IDS)" '[ -n "$DB_IDS" ] && [ "$DB_IDS" = "$HDR_IDS" ]'

docker rm -f e2e-pg >/dev/null; docker network rm e2e-net >/dev/null

read CP CF < "$WORK/counts.txt"
echo
echo "=================================================================="
echo " 결과: PASS $((CP + PASS)) / FAIL $((CF + FAIL))"
echo "=================================================================="
