#!/usr/bin/env bash
# Long buffering and failure scenarios (L1..L6), run inside the build container
# by e2e_long.sh. Intervals are scaled down (5-30s) to reproduce what happens
# with a 60 minute interval.
set -u
cd /work
PASS=0; FAIL=0
check() { if eval "$2"; then echo "    [PASS] $1"; PASS=$((PASS+1)); else echo "    [FAIL] $1"; FAIL=$((FAIL+1)); fi; }
info()  { echo "    [INFO] $*"; }
now()   { perl -MTime::HiRes=time -e 'printf "%.3f", time'; }
rel()   { perl -e 'printf "%.1f", $ARGV[0] - $ARGV[1]' "$(now)" "$T0"; }
ts()    { printf "  [+%5ss] %s\n" "$(rel)" "$*"; }
nfiles(){ find "$1" -name '*.flb' 2>/dev/null | wc -l | tr -d ' '; }
recv()  { rm -f "$2.mode"; perl recv.pl "$1" "$2" & }
section(){ echo; echo "=================================================================="; echo " $*"; echo "=================================================================="; }
fblog() { grep -E "$2" "$1" | sed 's/^\[[0-9\/ :.]*\] /    /'; }
params(){ echo "  ▶ 설정 값: batch_interval=${INTERVAL}s flush=${FLUSH}s batch_max_size=${MAX_SIZE} batch_hold_max_chunks=${MAX_CHUNKS} batch_retry_4xx=${RETRY_4XX} http.response_timeout=${RESP_TIMEOUT} storage.backlog.mem_limit=${BL_MEM}"; }
start_fb(){ $FB -c l.yaml > "$1" 2>&1 & FP=$!; }
stop_fb() { kill -TERM $FP 2>/dev/null; wait $FP 2>/dev/null; }
# gen <prefix> <count> <sleep> <sent file> [pad bytes]: posts numbered events,
# an event counts as sent only when the http input accepted it (201)
gen() {
    local p=$1 n=$2 d=$3 f=$4 pad=${5:-0} padstr i
    padstr=$(head -c "$pad" /dev/zero | tr '\0' 'x')
    # wait until the http input listens
    for i in $(seq 1 100); do curl -s -o /dev/null http://127.0.0.1:9880/ && break; sleep 0.1; done
    ( for i in $(seq 1 "$n"); do
        code=$(curl -s -o /dev/null -w "%{http_code}" -X POST -H 'Content-Type: application/json' \
               -d "{\"ev\":\"$p-$i\",\"pad\":\"$padstr\"}" http://127.0.0.1:9880/app)
        [ "$code" = 201 ] && echo "$p-$i" >> "$f"
        sleep "$d"
      done ) &
    GP=$!
}
# wait until every sent event arrived (or timeout in seconds)
delivered() { local i; for i in $(seq 1 "$3"); do perl verify.pl "$1" "$2" >/dev/null && return 0; sleep 1; done; return 1; }
defaults() { export FLUSH=1 BL_MEM=100M INTERVAL=10 MAX_SIZE=0 MAX_CHUNKS=512 RETRY_4XX=off RESP_TIMEOUT=10s; }
count_eq_len() { awk -F'\t' '$2 != $3 {bad=1} END {exit bad}' "$1"; }

rm -rf l*_* lstore counts_long.txt
FB=/build/bin/fluent-bit
section "공통 설정 (l.yaml, \${...} 는 시나리오마다 환경 변수로 지정)"
sed 's/^/    | /' l.yaml

# ------------------------------------------------------------------------------
section "L1. 수신 서버 장애가 여러 주기 동안 지속 (로그는 계속 유입)"
echo "  목적: 장애 동안 배치가 보관되고 가득 차면 나머지는 디스크에서 대기, 복구 후 유실·중복 없이 전부 전송되는지"
defaults; export INTERVAL=10 MAX_SIZE=4K; params
recv 9001 l1_recv.log; R=$!
T0=$(now); start_fb l1_fb.log; ts "fluent-bit 시작 (주기 10초)"
gen L1 150 0.2 l1_sent.txt; ts "로그 유입 시작 (초당 약 5건, 150건)"
sleep 4;  kill $R; wait $R 2>/dev/null; ts "수신 서버 다운 (+10, +20, +30초 주기 실패 예정)"
sleep 28; recv 9001 l1_recv.log; R=$!; ts "수신 서버 복구"
wait $GP; ts "로그 유입 종료 ($(wc -l < l1_sent.txt)건 입력)"
if delivered l1_sent.txt l1_recv.log 90; then ts "모든 이벤트 도착"; else ts "시간 초과"; fi
stop_fb; kill $R; ts "fluent-bit 종료"
echo "  ▶ 수신 서버가 받은 요청 (본문 생략)"; perl show.pl l1_recv.log $T0 brief | grep "→" | sed 's/→/  →/'
echo "  ▶ 결과 이벤트"; perl statuses.pl l1_fb.log | tee l1_status.txt | grep -v '^SEQ'
echo "  ▶ fluent-bit 로그 (배치 결과)"; fblog l1_fb.log "batch .* (sent|failed|dropped)|batch is full|retrying now" | head -20
echo "  ▶ 판정"
SEQ=$(grep '^SEQ' l1_status.txt | cut -d' ' -f2)
FULLS=$(grep -c "batch is full" l1_fb.log); ROUNDS=$(echo "$SEQ" | tr ',' '\n' | grep -c .)
V=$(perl verify.pl l1_sent.txt l1_recv.log)
check "유실 0 · 중복 0 ($V)" 'perl verify.pl l1_sent.txt l1_recv.log >/dev/null'
check "장애 동안 retry_next_interval 로 보관" '[[ "$SEQ" == *retry_next_interval* ]]'
check "폐기(dropped) 없음" '[[ "$SEQ" != *dropped* ]]'
check "'batch is full' 경고는 주기당 1번 이하 (경고 ${FULLS}번 / 주기 ${ROUNDS}번)" '[ "$FULLS" -le "$ROUNDS" ]'
check "모든 요청에서 count == events 배열 길이" 'count_eq_len l1_recv.log.tsv'
ORDER=$(awk -F'\t' '$7=="200"{print $4}' l1_recv.log.tsv | tr ',' '\n' | sed 's/.*-//' | awk 'NR>1 && $1<p {o=1} {p=$1} END {print o ? "섞임" : "유지"}')
info "요청 간 이벤트 순서: ${ORDER} (가득 차서 엔진 재시도로 넘어간 청크는 재시도 순서대로 도착, 청크 안의 순서는 유지)"

# ------------------------------------------------------------------------------
section "L2. 수신 서버가 413 (요청이 너무 큼) 반환"
echo "  목적: 413 을 받아도 배치를 버리지 않고 보관하는지 (원래 4xx 는 폐기 대상)"
defaults; export INTERVAL=5; params
rm -rf lstore; recv 9001 l2_recv.log; R=$!; echo 413 > l2_recv.log.mode
T0=$(now); start_fb l2_fb.log; ts "fluent-bit 시작 (주기 5초), 수신 서버는 413 응답"
gen L2 20 0.05 l2_sent.txt; wait $GP; ts "로그 20건 입력"
sleep 11; echo 200 > l2_recv.log.mode; ts "수신 서버 정상화 (200 응답)"
if delivered l2_sent.txt l2_recv.log 20; then ts "모든 이벤트 도착"; else ts "시간 초과"; fi
stop_fb; kill $R
echo "  ▶ 결과 이벤트"; perl statuses.pl l2_fb.log | tee l2_status.txt | grep -v '^SEQ'
echo "  ▶ fluent-bit 로그"; fblog l2_fb.log "413|batch .* (sent|failed|dropped)" | head -8
echo "  ▶ 판정"
SEQ=$(grep '^SEQ' l2_status.txt | cut -d' ' -f2)
V=$(perl verify.pl l2_sent.txt l2_recv.log)
check "유실 0 · 중복 0 ($V)" 'perl verify.pl l2_sent.txt l2_recv.log >/dev/null'
check "413 을 받은 주기는 retry_next_interval (http=413)" 'grep -q "retry_next_interval.*http=413" l2_status.txt'
check "폐기(dropped) 없음" '[[ "$SEQ" != *dropped* ]]'
check "원인과 조치가 에러 로그에 남음 (set 'batch_max_size' below the receiver limit)" 'grep -q "rejected with HTTP 413, set .batch_max_size. below the receiver limit" l2_fb.log'

# ------------------------------------------------------------------------------
section "L3. 수신 서버가 401 (인증 오류) 반환: 기본값 vs batch_retry_4xx"
echo "  목적: 기본값은 기존 out_http 처럼 4xx 배치를 폐기하고 그 사실을 로그·이벤트로 남기는지,"
echo "        batch_retry_4xx: on 이면 보관했다가 인증이 복구된 뒤 보내는지"
defaults; export INTERVAL=5; params
echo "  -- a. 기본값 (batch_retry_4xx: off)"
rm -rf lstore; recv 9001 l3a_recv.log; R=$!; echo 401 > l3a_recv.log.mode
T0=$(now); start_fb l3a_fb.log; ts "fluent-bit 시작, 수신 서버는 401 응답"
gen L3a 10 0.05 l3a_sent.txt; wait $GP; ts "로그 10건 입력"
sleep 6; echo 200 > l3a_recv.log.mode; ts "인증 복구 (200 응답)"
gen L3b 5 0.05 l3b_sent.txt; wait $GP; ts "로그 5건 추가 입력"
delivered l3b_sent.txt l3a_recv.log 15 >/dev/null
stop_fb; kill $R
echo "  ▶ 결과 이벤트"; perl statuses.pl l3a_fb.log | tee l3a_status.txt | grep -v '^SEQ'
echo "  ▶ fluent-bit 로그"; fblog l3a_fb.log "batch .* (sent|failed|dropped)" | head -4
echo "  -- b. batch_retry_4xx: on"
export RETRY_4XX=on; params
rm -rf lstore; recv 9001 l3c_recv.log; R=$!; echo 401 > l3c_recv.log.mode
T0=$(now); start_fb l3c_fb.log; ts "fluent-bit 시작, 수신 서버는 401 응답"
gen L3c 10 0.05 l3c_sent.txt; wait $GP; ts "로그 10건 입력"
sleep 6; echo 200 > l3c_recv.log.mode; ts "인증 복구 (200 응답)"
if delivered l3c_sent.txt l3c_recv.log 15; then ts "모든 이벤트 도착"; else ts "시간 초과"; fi
stop_fb; kill $R
echo "  ▶ 결과 이벤트"; perl statuses.pl l3c_fb.log | tee l3c_status.txt | grep -v '^SEQ'
echo "  ▶ fluent-bit 로그"; fblog l3c_fb.log "batch .* (sent|failed|dropped)" | head -4
echo "  ▶ 판정"
check "a: 401 배치는 dropped 로 기록 (http=401)" 'grep -q "dropped.*http=401" l3a_status.txt'
check "a: 폐기 사실이 에러 로그에 남음 (... are lost)" 'grep -q "dropped: HTTP status 401.* are lost" l3a_fb.log'
check "a: 인증 복구 뒤 새 로그는 정상 전송 ($(perl verify.pl l3b_sent.txt l3a_recv.log))" 'perl verify.pl l3b_sent.txt l3a_recv.log >/dev/null'
info "a: 401 을 받은 로그는 기본값에서 폐기됨 (설계대로) → $(perl verify.pl l3a_sent.txt l3a_recv.log)"
check "b: batch_retry_4xx 면 retry_next_interval 로 보관 (http=401)" 'grep -q "retry_next_interval.*http=401" l3c_status.txt'
check "b: 인증 복구 뒤 전부 전송, 유실 0 · 중복 0 ($(perl verify.pl l3c_sent.txt l3c_recv.log))" 'perl verify.pl l3c_sent.txt l3c_recv.log >/dev/null'

# ------------------------------------------------------------------------------
section "L4. 대량 backlog 가 있는 상태에서 크래시 → 재시작"
echo "  목적: 재시작 후 디스크의 backlog 가 첫 주기에 전송되는지, storage.backlog.mem_limit 을 아주 작게(16K) 해도 같은지"
for BLM in 16K 100M; do
    defaults; export INTERVAL=30 BL_MEM=$BLM; params
    rm -rf lstore; recv 9001 l4_${BLM}_recv.log; R=$!
    T0=$(now); start_fb l4_${BLM}_fb1.log; ts "fluent-bit 시작 (주기 30초)"
    gen L4$BLM 200 0.01 l4_${BLM}_sent.txt 400; wait $GP; ts "로그 200건 입력 (건당 약 400바이트)"
    sleep 2; ts "청크 파일 $(nfiles lstore)개 보관 중 → kill -9"; kill -9 $FP; wait $FP 2>/dev/null
    export INTERVAL=5
    T0=$(now); start_fb l4_${BLM}_fb2.log; ts "재시작 (주기 5초, storage.backlog.mem_limit=$BLM)"
    if delivered l4_${BLM}_sent.txt l4_${BLM}_recv.log 120; then ts "모든 이벤트 도착"; else ts "시간 초과"; fi
    stop_fb; kill $R
    perl statuses.pl l4_${BLM}_fb2.log > l4_${BLM}_status.txt
    eval "N_${BLM}=\$(grep -c '^    success' l4_${BLM}_status.txt)"
    echo "  ▶ 재시작 후 결과 이벤트"; grep -v '^SEQ' l4_${BLM}_status.txt | grep -v "empty" | head -12
done
echo "  ▶ 판정"
check "mem_limit 16K: 유실 0 · 중복 0 ($(perl verify.pl l4_16K_sent.txt l4_16K_recv.log))" 'perl verify.pl l4_16K_sent.txt l4_16K_recv.log >/dev/null'
check "mem_limit 100M: 유실 0 · 중복 0 ($(perl verify.pl l4_100M_sent.txt l4_100M_recv.log))" 'perl verify.pl l4_100M_sent.txt l4_100M_recv.log >/dev/null'
check "mem_limit 100M: 재시작 후 첫 주기에 backlog 전부 전송 (성공 ${N_100M}번)" '[ "$N_100M" -eq 1 ]'
check "mem_limit 16K: 한도가 작아도 첫 주기에 backlog 전부 전송 (성공 ${N_16K}번)" '[ "$N_16K" -eq 1 ]'

# ------------------------------------------------------------------------------
section "L5. 붙잡는 청크 수 상한 (batch_hold_max_chunks)"
echo "  목적: 상한을 넘는 청크는 디스크에서 대기했다가 이후 주기에 유실 없이 전송되는지"
defaults; export INTERVAL=10 FLUSH=0.5 MAX_CHUNKS=5; params
rm -rf lstore; recv 9001 l5_recv.log; R=$!
T0=$(now); start_fb l5_fb.log; ts "fluent-bit 시작 (주기 10초, flush 0.5초 → 주기당 청크 약 20개, 상한 5)"
gen L5 100 0.2 l5_sent.txt; wait $GP; ts "로그 $(wc -l < l5_sent.txt)건 입력"
if delivered l5_sent.txt l5_recv.log 90; then ts "모든 이벤트 도착"; else ts "시간 초과"; fi
stop_fb; kill $R
echo "  ▶ fluent-bit 로그"; fblog l5_fb.log "chunks per tag|batch .* sent|batch is full" | head -10
echo "  ▶ 판정"
MAXC=$(grep -oE "sent: [0-9]+ records \(([0-9]+) chunks\)" l5_fb.log | grep -oE "\(([0-9]+)" | tr -d '(' | sort -n | tail -1)
check "시작 시 설정 점검 경고 (chunks per tag ... more than batch_hold_max_chunks)" 'grep -q "more than batch_hold_max_chunks" l5_fb.log'
check "한 배치의 청크 수는 상한 이하 (최대 ${MAXC}개)" '[ "${MAXC:-99}" -le 5 ]'
check "유실 0 · 중복 0 ($(perl verify.pl l5_sent.txt l5_recv.log))" 'perl verify.pl l5_sent.txt l5_recv.log >/dev/null'

# ------------------------------------------------------------------------------
section "L6. 연결은 받지만 응답하지 않는 수신 서버에서 종료"
echo "  목적: 종료가 응답 제한 시간 때문에 얼마나 늦어지는지, 보내지 못한 로그가 재시작 후 전송되는지"
defaults; export INTERVAL=30 RESP_TIMEOUT=5s; params
rm -rf lstore; recv 9001 l6_recv.log; R=$!; echo hang > l6_recv.log.mode
T0=$(now); start_fb l6_fb1.log; ts "fluent-bit 시작, 수신 서버는 응답하지 않음"
gen L6 10 0.05 l6_sent.txt; wait $GP; ts "로그 10건 입력"
sleep 2; T_TERM=$(now); stop_fb; SD=$(perl -e 'printf "%.1f", $ARGV[0] - $ARGV[1]' "$(now)" "$T_TERM"); ts "SIGTERM → ${SD}초 만에 종료, 청크 파일 $(nfiles lstore)개 남음"
kill $R; wait $R 2>/dev/null; recv 9001 l6_recv2.log; R=$!
export INTERVAL=5; start_fb l6_fb2.log; ts "수신 서버 정상화 후 재시작"
if delivered l6_sent.txt l6_recv2.log 30; then ts "모든 이벤트 도착"; else ts "시간 초과"; fi
stop_fb; kill $R
echo "  ▶ fluent-bit 로그 (종료 중)"; fblog l6_fb1.log "batch .* (sent|failed|dropped)|stopping|stopped" | head -6
echo "  ▶ 판정"
check "종료 시간이 grace(10초) + 응답 제한(5초) 이내 (${SD}초)" 'lt() { awk -v a="$1" -v b="$2" "BEGIN{exit !(a<b)}"; }; lt "$SD" 16'
check "보내지 못한 로그는 재시작 후 전송, 유실 0 · 중복 0 ($(perl verify.pl l6_sent.txt l6_recv2.log))" 'perl verify.pl l6_sent.txt l6_recv2.log >/dev/null'

echo "$PASS $FAIL" > counts_long.txt
