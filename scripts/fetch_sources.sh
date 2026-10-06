#!/bin/bash
# Download + unpack every Husk dependency into third_party/sources and third_party/build.
set -u
HUSK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$HUSK_ROOT/scripts/sources.sh"
DL="$HUSK_ROOT/third_party/sources"
SRC="$HUSK_ROOT/third_party/build"
mkdir -p "$DL" "$SRC"

# Print the SHA-256 of a file, with whichever tool this system has (macOS ships
# shasum, Linux sha256sum).
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        echo "neither sha256sum nor shasum is available" >&2
        return 1
    fi
}

# verify FILE EXPECTED: 0 if FILE hashes to EXPECTED. An empty EXPECTED only warns.
verify() {
    local file="$1" want="$2" got
    if [ -z "$want" ]; then
        echo "[WARN] $(basename "$file"): no SHA-256 pin in sources.sh; NOT verified" >&2
        return 0
    fi
    got="$(sha256_of "$file")" || return 1
    if [ "$got" != "$want" ]; then
        echo "[FAIL] $(basename "$file"): SHA-256 mismatch" >&2
        echo "       expected $want" >&2
        echo "       got      $got" >&2
        return 1
    fi
    echo "[ ok ] $(basename "$file") sha256 verified"
}

# fetch URL SHA256: download (unless present) and verify. A file that fails
# verification -- freshly downloaded or left from an earlier run -- is moved
# aside to FILE.bad and never unpacked.
fetch() {
    local url="$1" want="$2" file
    file="$DL/$(basename "$url")"
    if [ -s "$file" ]; then
        echo "[have] $(basename "$file")"
    else
        echo "[get ] $(basename "$file")"
        curl -fL --retry 3 --retry-delay 5 -o "$file.part" "$url" || { echo "[FAIL] $url"; return 1; }
        mv "$file.part" "$file"
    fi
    if ! verify "$file" "$want"; then
        mv -f "$file" "$file.bad"
        rm -f "$SRC/.unpacked-$(basename "$file")"
        echo "       moved to $file.bad; refusing to use it" >&2
        return 1
    fi
}

unpack() {
    local file stamp
    file="$DL/$(basename "$1")"
    stamp="$SRC/.unpacked-$(basename "$file")"
    [ -f "$stamp" ] && { echo "[skip] unpack $(basename "$file")"; return 0; }
    echo "[tar ] $(basename "$file")"
    tar -xf "$file" -C "$SRC" || return 1
    touch "$stamp"
}

# URL and SHA-256 pairs, from sources.sh.
set -- \
    "$FFI_SRC" "${FFI_SHA256:-}" \
    "$ICONV_SRC" "${ICONV_SHA256:-}" \
    "$GETTEXT_SRC" "${GETTEXT_SHA256:-}" \
    "$GLIB_SRC" "${GLIB_SHA256:-}" \
    "$PIXMAN_SRC" "${PIXMAN_SHA256:-}" \
    "$SLIRP_SRC" "${SLIRP_SHA256:-}" \
    "$QEMU_SRC" "${QEMU_SHA256:-}"

rc=0
while [ $# -ge 2 ]; do
    # Only unpack what was fetched and verified.
    if fetch "$1" "$2"; then
        unpack "$1" || rc=1
    else
        rc=1
    fi
    shift 2
done

if [ ! -d "$SRC/libucontext" ]; then
    echo "[git ] libucontext"
    git clone "$LIBUCONTEXT_REPO" "$SRC/libucontext" >/dev/null 2>&1 \
        && git -C "$SRC/libucontext" checkout -q "$LIBUCONTEXT_COMMIT" \
        || { echo "[FAIL] libucontext"; rc=1; }
else
    echo "[skip] libucontext"
fi

echo "--- result rc=$rc ---"
ls -1 "$SRC"
exit $rc
