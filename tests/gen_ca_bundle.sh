#!/bin/bash
# gen_ca_bundle.sh -- plan 3.2: write tests/corpus/ca-bundle.pem from the
# host's OWN curl/OpenSSL bundle (NO network). Refuses output unless the
# pinned root ISRG Root X1 is present and the count exceeds the floor, so a
# stale or truncated bundle can never be committed as the deterministic
# fallback. Update procedure: re-run this when the Mozilla root set moves;
# update the fingerprint in tests/corpus/isrg_root_x1.pin (the ONE source of
# truth, read by both this script and tests/test_tls_roots.js); commit all.
set -eu

PIN="$(cat "$(dirname "$0")/corpus/isrg_root_x1.pin")"
FLOOR=100
OUT="tests/corpus/ca-bundle.pem"

SRC=""
for c in /etc/ssl/cert.pem /etc/ssl/certs/ca-certificates.crt \
         /etc/pki/tls/certs/ca-bundle.crt; do
  [ -f "$c" ] && SRC="$c" && break
done
[ -n "$SRC" ] || { echo "gen_ca_bundle: no host bundle found" >&2; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

awk '
  /^-----BEGIN CERTIFICATE-----/ { n++; f=1 }
  f { print > ("'"$TMP"'/c" n ".pem") }
  /^-----END CERTIFICATE-----/   { f=0 }
  END { print n }
' "$SRC" > "$TMP/count"

N="$(cat "$TMP/count")"
[ "$N" -gt "$FLOOR" ] || { echo "gen_ca_bundle: only $N roots in $SRC (< $FLOOR)" >&2; exit 1; }

FOUND=0
for i in $(seq 1 "$N"); do
  FP="$(openssl x509 -in "$TMP/c$i.pem" -noout -fingerprint -sha256 2>/dev/null \
        | sed 's/.*=//' | tr -d ':' | tr '[:upper:]' '[:lower:]')"
  [ "$FP" = "$PIN" ] && FOUND=1
done
[ "$FOUND" = 1 ] || { echo "gen_ca_bundle: pinned root ISRG Root X1 not in $SRC" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"
cp "$SRC" "$OUT"
echo "gen_ca_bundle: wrote $OUT from $SRC ($N roots, pinned root present)"
