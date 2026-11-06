#!/bin/bash
# Builds /src into /out/<label>. Flags follow dockerfiles/Dockerfile, except Kafka
# and WASM, which are off to keep builds short. FLB_RELEASE sets -O2 through a
# normal variable, so CMakeCache still shows FLB_DEBUG=ON. flags.make is the proof.
set -u
LABEL="${1:-base}"
cmake -S /src -B /build -DFLB_RELEASE=On -DFLB_SIMD=On -DFLB_JEMALLOC=On -DFLB_TLS=On \
    -DFLB_LUAJIT=On -DFLB_FILTER_LUA=On -DFLB_KAFKA=Off -DFLB_IN_KAFKA=Off -DFLB_OUT_KAFKA=Off \
    -DFLB_WASM=Off -DFLB_OUT_PGSQL=Off -DFLB_TESTS_RUNTIME=Off -DFLB_TESTS_INTERNAL=Off \
    -DFLB_EXAMPLES=Off -DFLB_SHARED_LIB=Off > /out/cmake.log 2>&1 \
    || { grep -E 'CMake Error' -A4 /out/cmake.log | head -20; echo "cmake failed"; exit 1; }
cmake --build /build -j"$(nproc)" --target fluent-bit-bin > /out/make.log 2>&1 \
    || { grep -E 'error:|undefined reference|\*\*\*' /out/make.log | head -15; echo "make failed"; exit 1; }
grep -q -- '-O2' /build/src/CMakeFiles/fluent-bit-static.dir/flags.make \
    || { echo "binary is not optimized"; exit 1; }
mkdir -p "/out/$LABEL" /out/tools
cp /build/bin/fluent-bit "/out/$LABEL/fluent-bit"
cc -O2 -Wall -pthread -o /out/tools/gen /h/gen.c || exit 1
cc -O2 -Wall -pthread -o /out/tools/sink /h/sink.c || exit 1
echo "BUILD OK label=$LABEL $(/out/$LABEL/fluent-bit --version | head -1) lua=$(/out/$LABEL/fluent-bit -h | grep -c 'lua ')"
