#!/bin/sh
# Run the repository test suites related to uuid_file and write their
# summaries into report/suites.txt for build_report.py.
#
#   BUILD_DIR=build ./collect_suites.sh
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../../.." && pwd)
BUILD_DIR=${BUILD_DIR:-build}
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$REPO/$BUILD_DIR" ;; esac
PYTHON="$REPO/tests/integration/.venv/bin/python"
OUT="$HERE/report/suites.txt"

export FLUENT_BIT_BINARY="${FLUENT_BIT_BINARY:-$BUILD_DIR/bin/fluent-bit}"
mkdir -p "$HERE/report"
: > "$OUT"

for t in out_file_uuid out_file out_file_rotation out_http; do
    r=$("$BUILD_DIR/bin/flb-rt-$t" 2>&1)
    n=$(printf '%s\n' "$r" | grep -cE '^Test .*\.\.\.')
    s=$(printf '%s\n' "$r" | grep -E 'SUCCESS|FAILED:' | tail -1)
    echo "runtime|flb-rt-$t|$n|$s" >> "$OUT"
done

cd "$REPO"
s=$("$PYTHON" -m pytest tests/integration/scenarios/out_file -q -p no:logging 2>&1 | grep -E 'passed|failed' | tail -1)
echo "integration|out_file (일반)|5|$s" >> "$OUT"

if [ "$(uname)" = "Darwin" ]; then
    s=$(LEAKS=1 LEAKS_STRICT=1 "$PYTHON" -m pytest tests/integration/scenarios/out_file -q -p no:logging 2>&1 | grep -E 'passed|failed' | tail -1)
    echo "integration|out_file (macOS Leaks)|5|$s" >> "$OUT"
else
    s=$(VALGRIND=1 VALGRIND_STRICT=1 "$PYTHON" -m pytest tests/integration/scenarios/out_file -q -p no:logging 2>&1 | grep -E 'passed|failed' | tail -1)
    echo "integration|out_file (Valgrind)|5|$s" >> "$OUT"
fi

cat "$OUT"
