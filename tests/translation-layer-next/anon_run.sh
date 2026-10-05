#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The MADV_DONTNEED table (src/translation-layer-next/husk-tl-anon.c), runnable
# on a Linux host: once under ASan, UBSan and LSan, and once under TSan for the
# two-thread map/unmap run.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../../src/translation-layer-next" && pwd)
OUT=${OUT:-$(mktemp -d)}
mkdir -p "$OUT"
CFLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror -I$SRC"

echo "== anon table, ASan/UBSan/LSan"
cc $CFLAGS -fsanitize=address,undefined -fno-sanitize-recover=all -o "$OUT/anon_test_asan" \
    "$HERE/anon_test.c" -lpthread
ASAN_OPTIONS=detect_leaks=1 "$OUT/anon_test_asan"

echo "== anon table, TSan"
cc $CFLAGS -fsanitize=thread -o "$OUT/anon_test_tsan" "$HERE/anon_test.c" -lpthread
TSAN_OPTIONS=halt_on_error=1 "$OUT/anon_test_tsan"
