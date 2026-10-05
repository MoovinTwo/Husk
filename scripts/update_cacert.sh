#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Refresh the root-certificate bundle the native runtime hands to Android games
# (src/app/Husk/Resources/cacert.pem; see docs/04-translation-layer.md, "TLS roots").
#
# Downloads curl's extraction of Mozilla's CA store and its published SHA-256,
# checks one against the other, sanity-checks the contents, and only then
# replaces the bundled file. Both come from curl.se over HTTPS, so the hash
# guards against a truncated or corrupted transfer, not against curl.se itself.
#
#   scripts/update_cacert.sh [TARGET]     (TARGET defaults to the bundled file)
set -eu

HUSK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="${1:-$HUSK_ROOT/src/app/Husk/Resources/cacert.pem}"
URL="https://curl.se/ca/cacert.pem"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        echo "error: neither sha256sum nor shasum is available" >&2
        exit 1
    fi
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

echo "[get ] $URL"
curl -fsSL --retry 3 -o "$TMP/cacert.pem" "$URL"
curl -fsSL --retry 3 -o "$TMP/cacert.pem.sha256" "$URL.sha256"

want="$(cut -d' ' -f1 < "$TMP/cacert.pem.sha256")"
got="$(sha256_of "$TMP/cacert.pem")"
case "$want" in
    *[!0-9a-f]*|"") echo "error: unreadable checksum file from $URL.sha256" >&2; exit 1 ;;
esac
if [ "$want" != "$got" ]; then
    echo "error: SHA-256 mismatch for cacert.pem; bundle left unchanged" >&2
    echo "       expected $want" >&2
    echo "       got      $got" >&2
    exit 1
fi

count="$(grep -c -- '-----BEGIN CERTIFICATE-----' "$TMP/cacert.pem" || true)"
if [ "$count" -lt 100 ]; then
    echo "error: only $count certificates in the download; bundle left unchanged" >&2
    exit 1
fi

date_line="$(grep -m1 '^## Certificate data from Mozilla' "$TMP/cacert.pem" || true)"
if [ -f "$TARGET" ] && cmp -s "$TMP/cacert.pem" "$TARGET"; then
    echo "[same] $TARGET is already current ($count certificates)"
    exit 0
fi
cp "$TMP/cacert.pem" "$TARGET.new"
mv "$TARGET.new" "$TARGET"
echo "[ ok ] $TARGET: $count certificates, sha256 $got"
[ -n "$date_line" ] && echo "       ${date_line#\#\# }"
exit 0
