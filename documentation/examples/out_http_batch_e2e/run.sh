#!/usr/bin/env bash
# Runs inside the build container: builds fluent-bit from /src and runs T1..T3.
# Called by e2e.sh; T3 database checks are done by e2e.sh on the host.
set -u
cd /work

PASS=0; FAIL=0
check() { if eval "$2"; then echo "    [PASS] $1"; PASS=$((PASS+1)); else echo "    [FAIL] $1"; FAIL=$((FAIL+1)); fi; }
info()  { echo "    [INFO] $*"; }
now()   { perl -MTime::HiRes=time -e 'printf "%.3f", time'; }
rel()   { perl -e 'printf "%.1f", $ARGV[0] - $ARGV[1]' "$(now)" "$T0"; }
ts()    { printf "  [+%5ss] %s\n" "$(rel)" "$*"; }
post()  { for e in "$@"; do curl -s -o /dev/null -X POST -H 'Content-Type: application/json' -d "{\"ev\":\"$e\"}" http://127.0.0.1:9880/app; done; ts "로그 전송: $*"; }
nfiles(){ find "$1" -name '*.flb' 2>/dev/null | wc -l | tr -d ' '; }
recv()  { perl recv.pl "$1" "$2" & }
field() { awk -F'\t' -v r="$2" -v c="$3" 'NR==r{print $c}' "$1"; }
lt()    { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a<b)}'; }
gt()    { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a>b)}'; }
section(){ echo; echo "=================================================================="; echo " $*"; echo "=================================================================="; }
showcfg(){ echo "  ▶ 설정 ($1)"; sed 's/^/    | /' "$1"; }
fblog() { grep -E "$2" "$1" | sed 's/^\[[0-9\/ :.]*\] /    /'; }

rm -rf t1_* t2_* t3_* storage2 storage3 counts.txt

section "빌드"
mkdir -p /srcrw /build
tar -C /src --exclude=./build --exclude=./.git -cf - . | tar -C /srcrw -xf -
cd /build
cmake /srcrw -DFLB_MINIMAL=On -DFLB_OUT_HTTP=On -DFLB_IN_HTTP=On -DFLB_IN_STORAGE_BACKLOG=On \
      -DFLB_OUT_STDOUT=On -DFLB_CONFIG_YAML=On -DFLB_KAFKA=Off -DFLB_IN_EMITTER=On \
      -DFLB_OUT_PGSQL=On > cmake.log 2>&1 || { tail -20 cmake.log; exit 1; }
make -j"$(nproc)" fluent-bit-bin > make.log 2>&1 || { grep " error" make.log | head; exit 1; }
cd /work
FB=/build/bin/fluent-bit
echo "  $($FB --version | head -1)"
echo "  플러그인: out_http, in_http, in_emitter=$(grep -i '^FLB_IN_EMITTER:BOOL' /build/CMakeCache.txt | cut -d= -f2), out_pgsql=$(grep -i '^FLB_OUT_PGSQL:BOOL' /build/CMakeCache.txt | cut -d= -f2)"
echo "  수신 서버: recv.pl (요청 헤더·본문을 그대로 기록, 응답 후 연결 종료)"
echo "  로그 입력: curl -X POST http://127.0.0.1:9880/app -d '{\"ev\":\"e1\"}'"

# ------------------------------------------------------------------------------
section "T1. JSON 봉투 + 주기 배치 (batch_interval 5초)"
echo "  목적: 5초 동안 들어온 로그가 요청 1건 {\"count\":N,\"events\":[...]} 로 나가고,"
echo "        보낼 데이터가 없는 주기에는 전송하지 않는지 확인"
showcfg t1.yaml
echo "  ▶ 진행"
recv 9001 t1_recv.log; R=$!
T0=$(now); $FB -c t1.yaml > t1_fb.log 2>&1 & FP=$!; ts "fluent-bit 시작 (전송 주기: +5초, +10초, +15초)"
sleep 1;    post e1 e2 e3 e4 e5
sleep 10.5; ts "+5초 주기 전송, +10초 주기는 보낼 데이터 없음"
post e6 e7
sleep 4.5;  ts "+15초 주기 전송"
kill -TERM $FP; wait $FP 2>/dev/null; kill $R; ts "fluent-bit 종료"
echo "  ▶ 수신 서버가 받은 요청"; perl show.pl t1_recv.log $T0
echo "  ▶ fluent-bit 로그"; fblog t1_fb.log "batch mode|HTTP status"
echo "  ▶ 판정"
N=$(wc -l < t1_recv.log.tsv | tr -d ' ')
check "요청은 2건뿐 (주기 3번 중 보낼 데이터가 없던 +10초 주기는 전송 안 함)" '[ "$N" -eq 2 ]'
check "1번째 요청: count=5, 배열 5건, 순서 e1~e5" '[ "$(field t1_recv.log.tsv 1 2)" = 5 ] && [ "$(field t1_recv.log.tsv 1 3)" = 5 ] && [ "$(field t1_recv.log.tsv 1 4)" = "e1,e2,e3,e4,e5" ]'
check "2번째 요청: count=2, 순서 e6,e7" '[ "$(field t1_recv.log.tsv 2 2)" = 2 ] && [ "$(field t1_recv.log.tsv 2 4)" = "e6,e7" ]'
check "2번째 요청은 +15초 주기에 전송 (+$(field t1_recv.log.tsv 2 1)초)" 'gt "$(field t1_recv.log.tsv 2 1)" 13'
check "Content-Type: application/json" '[ "$(cut -f6 t1_recv.log.tsv | sort -u)" = "application/json" ]'

# ------------------------------------------------------------------------------
section "T2. 유실 방지 모드 (filesystem) + 같은 입력을 쓰는 다른 output"
echo "  목적: 전송이 성공해야만 청크 파일이 지워지는지, 강제 종료 후 재시작하면 다시 보내는지,"
echo "        정상 종료 시 즉시 보내는지, 배치가 청크를 붙잡는 동안 다른 output 이 재전송하지 않는지"
showcfg t2.yaml
echo "  ▶ 진행"
recv 9001 t2_batch.log; RB=$!; recv 9002 t2_other.log; RO=$!
echo "  -- A. 정상 주기 (batch_interval 10초)"
T0=$(now); BATCH=10 $FB -c t2.yaml > t2a_fb.log 2>&1 & FP=$!; ts "fluent-bit 시작 (전송 주기: +10초)"
sleep 1; post h1 h2 h3
sleep 3; A_HOLD=$(nfiles storage2); ts "배치 대기 중 디스크의 청크 파일: ${A_HOLD}개"
sleep 7; A_SENT=$(nfiles storage2); ts "+10초 주기 전송 후 청크 파일: ${A_SENT}개"
echo "  -- B. 청크를 붙잡은 상태에서 kill -9 (강제 종료) → 재시작 (batch_interval 5초)"
post h4 h5
sleep 3; B_HOLD=$(nfiles storage2); ts "배치 대기 중 청크 파일: ${B_HOLD}개"
kill -9 $FP; wait $FP 2>/dev/null; B_KILL=$(nfiles storage2); ts "kill -9 후 청크 파일: ${B_KILL}개"
BATCH=5 $FB -c t2.yaml > t2b_fb.log 2>&1 & FP=$!; ts "fluent-bit 재시작"
sleep 7; B_SENT=$(nfiles storage2); ts "재시작 후 주기 전송 뒤 청크 파일: ${B_SENT}개"
echo "  -- C. 청크를 붙잡은 상태에서 SIGTERM (정상 종료)"
post h6
sleep 2; C_HOLD=$(nfiles storage2); ts "배치 대기 중 청크 파일: ${C_HOLD}개"
T_TERM=$(rel); kill -TERM $FP; wait $FP 2>/dev/null; C_END=$(nfiles storage2); ts "SIGTERM → 종료 완료, 청크 파일: ${C_END}개"
kill $RB $RO
echo "  ▶ batch_out 수신 서버가 받은 요청"; perl show.pl t2_batch.log $T0
echo "  ▶ other_out 수신 서버가 받은 요청"; perl show.pl t2_other.log $T0
echo "  ▶ fluent-bit 로그 (재시작 후)"; fblog t2b_fb.log "backlog chunk"
echo "  ▶ 판정"
check "A: 배치 대기 중에는 청크 파일이 디스크에 남아 있음 (${A_HOLD}개)" '[ "$A_HOLD" -ge 1 ]'
check "A: 주기 전송 성공 후 청크 파일 삭제 (${A_SENT}개)" '[ "$A_SENT" -eq 0 ]'
check "A: batch_out 이 +10초 주기에 count=3 [h1,h2,h3] 1건 수신" '[ "$(field t2_batch.log.tsv 1 2)" = 3 ] && [ "$(field t2_batch.log.tsv 1 4)" = "h1,h2,h3" ]'
check "A: other_out 은 주기와 무관하게 바로 수신 (+$(field t2_other.log.tsv 1 1)초)" 'lt "$(field t2_other.log.tsv 1 1)" 3'
check "B: kill -9 후에도 청크 파일이 디스크에 남아 있음 (${B_KILL}개)" '[ "$B_KILL" -ge 1 ]'
check "B: 재시작 후 batch_out 이 [h4,h5] 를 정확히 1번 수신" '[ "$(grep -c "h4,h5" t2_batch.log.tsv)" -eq 1 ]'
check "B: 재전송 성공 후 청크 파일 삭제 (${B_SENT}개)" '[ "$B_SENT" -eq 0 ]'
check "C: SIGTERM 즉시 [h6] 전송 (SIGTERM +${T_TERM}초 → 수신 +$(field t2_batch.log.tsv 3 1)초)" '[ "$(field t2_batch.log.tsv 3 4)" = h6 ] && lt "$(field t2_batch.log.tsv 3 1)" "$(awk -v t="$T_TERM" "BEGIN{print t+2}")"'
check "C: 종료 후 청크 파일 삭제 (${C_END}개)" '[ "$C_END" -eq 0 ]'
check "배치가 청크를 붙잡는 동안 other_out 은 재전송하지 않음 ([h1,h2,h3] 1번)" '[ "$(grep -c "h1,h2,h3" t2_other.log.tsv)" -eq 1 ]'
info "크래시 후 재시작 시 other_out 이 [h4,h5] 를 $(grep -c "h4,h5" t2_other.log.tsv)번 수신 → 알려진 동작 (가이드 7.3, 피하려면 7.4 격리 구성)"

# ------------------------------------------------------------------------------
section "T3. 결과 이벤트 → PostgreSQL (batch_interval 3초)"
echo "  목적: 주기마다 결과(success / empty / retry_next_interval)가 PostgreSQL 에 1행씩 저장되고,"
echo "        요청의 X-Batch-Id 헤더와 DB 의 batch_id 가 같은지 확인 (DB 확인은 아래 호스트 단계)"
showcfg t3.yaml
echo "  ▶ 진행"
recv 9001 t3_recv.log; R=$!
T0=$(now); $FB -c t3.yaml > t3_fb.log 2>&1 & FP=$!; ts "fluent-bit 시작 (전송 주기: 약 3초마다)"
sleep 1;   post s1 s2 s3
sleep 5.5; kill $R; wait $R 2>/dev/null; ts "수신 서버 중단 (다음 주기 전송 실패 예정)"
post s4 s5
sleep 6;   recv 9001 t3_recv.log; R=$!; ts "수신 서버 재시작"
sleep 4;   kill -TERM $FP; wait $FP 2>/dev/null; kill $R; ts "fluent-bit 종료"
echo "  ▶ 수신 서버가 받은 요청"; perl show.pl t3_recv.log $T0
echo "  ▶ fluent-bit 로그"; fblog t3_fb.log "batch_status\] initializing|batch mode|pgsql.*OK|retrying|failed to send"

echo "$PASS $FAIL" > counts.txt
