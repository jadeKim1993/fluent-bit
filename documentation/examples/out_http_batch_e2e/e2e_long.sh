#!/usr/bin/env bash
# Long buffering and failure scenarios of the out_http batching features.
#
#   bash documentation/examples/out_http_batch_e2e/e2e_long.sh
#
# Requires docker with the image fb-builder-pg (fluent-bit build tools).
set -u
KIT="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$KIT/../../.." && pwd)"
WORK="${WORK:-$(mktemp -d)}"
IMAGE="${IMAGE:-fb-builder-pg:latest}"
cp "$KIT"/*.pl "$KIT"/*.sh "$KIT"/*.yaml "$WORK"/

echo "=================================================================="
echo " out_http 배치 기능: 장시간 버퍼링 · 장애 시나리오 테스트"
echo "=================================================================="
echo "  실행 시각 : $(date '+%Y-%m-%d %H:%M:%S %Z')"
echo "  소스      : $(git -C "$SRC" log --oneline -1) (+ 작업 트리 변경분)"
echo "  실행 환경 : docker $(docker version --format '{{.Server.Version}} ({{.Server.Os}}/{{.Server.Arch}})'), 이미지 $IMAGE"
echo "  주기      : 60분 주기의 동작을 5~30초 주기로 축소해 재현"

docker run --rm --name fb-e2e-long -v "$SRC":/src:ro -v fb-linux-build:/build \
       -v "$WORK":/work "$IMAGE" bash -c '
  set -e
  mkdir -p /srcrw /build
  tar -C /src --exclude=./build --exclude=./.git -cf - . | tar -C /srcrw -xf -
  cd /build
  cmake /srcrw -DFLB_MINIMAL=On -DFLB_OUT_HTTP=On -DFLB_IN_HTTP=On -DFLB_IN_STORAGE_BACKLOG=On \
        -DFLB_OUT_STDOUT=On -DFLB_CONFIG_YAML=On -DFLB_KAFKA=Off -DFLB_IN_EMITTER=On \
        -DFLB_OUT_PGSQL=On > cmake.log 2>&1
  make -j"$(nproc)" fluent-bit-bin > make.log 2>&1
  echo "  빌드      : $(/build/bin/fluent-bit --version | head -1)"
  bash /work/run_long.sh'

read P F < "$WORK/counts_long.txt"
echo
echo "=================================================================="
echo " 결과: PASS $P / FAIL $F"
echo "=================================================================="
