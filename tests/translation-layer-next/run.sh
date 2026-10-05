#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Tests for src/translation-layer-next, runnable on a Linux host. Each one is
# built and run twice: under ASan, UBSan and LSan, which see memory used after
# it is freed or never freed, and under TSan, which sees two threads racing.
#
# Needs a C compiler with the sanitizers (gcc or clang).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../../src/translation-layer-next" && pwd)
OUT=${OUT:-$(mktemp -d)}
mkdir -p "$OUT"
CFLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror -I$SRC -pthread"

# run NAME SOURCE...
run() {
    name=$1; shift
    echo "== $name, address and undefined"
    cc $CFLAGS -fsanitize=address,undefined -fno-sanitize-recover=all -o "$OUT/$name-asan" "$@"
    "$OUT/$name-asan"
    echo "== $name, thread"
    cc $CFLAGS -fsanitize=thread -o "$OUT/$name-tsan" "$@"
    TSAN_OPTIONS="halt_on_error=1 ${TSAN_OPTIONS:-}" "$OUT/$name-tsan"
}

run jni_test "$HERE/jni_test.c"
