#!/bin/sh
# Fetch the JSON-Schema-Test-Suite vectors that tests/schema/test_schema_conformance.js runs.
#
# The corpus is not committed (tests/corpus/jsonschema/ is gitignored); without it the conformance
# suite prints a SKIP line naming this script. The commit is pinned so every machine runs the same
# vectors: a different commit is a different oracle, so move the pin deliberately and re-run the suite.
#
#   tools/fetch-jsonschema-suite.sh            fetch into tests/corpus/jsonschema/ (replacing it)

set -eu
COMMIT=89e83a74014617ffbe79a56d83d14d054cd11417
URL="https://codeload.github.com/json-schema-org/JSON-Schema-Test-Suite/tar.gz/$COMMIT"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST="$ROOT/tests/corpus/jsonschema"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/jsts.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

command -v curl >/dev/null 2>&1 || { echo "fetch-jsonschema-suite: curl not found" >&2; exit 1; }
echo "fetch-jsonschema-suite: $URL"
curl -fsSL "$URL" -o "$WORK/suite.tar.gz"
mkdir "$WORK/x"
tar -xzf "$WORK/suite.tar.gz" -C "$WORK/x"
SRC="$WORK/x/JSON-Schema-Test-Suite-$COMMIT"
for need in tests/draft2020-12 remotes/draft2020-12 LICENSE; do
    [ -e "$SRC/$need" ] || { echo "fetch-jsonschema-suite: $need missing from the archive" >&2; exit 1; }
done

rm -rf "$DEST"
mkdir -p "$DEST/remotes"
cp -R "$SRC/tests/draft2020-12" "$DEST/draft2020-12"
cp -R "$SRC/remotes/draft2020-12" "$DEST/remotes/draft2020-12"
cp "$SRC/LICENSE" "$DEST/LICENSE"
cat > "$DEST/SOURCE.txt" <<EOF
JSON-Schema-Test-Suite (MIT, see LICENSE), commit $COMMIT:
  tests/draft2020-12 -> draft2020-12/
  remotes/draft2020-12 -> remotes/draft2020-12/ (served as http://localhost:1234/draft2020-12/)
EOF
echo "fetch-jsonschema-suite: $(find "$DEST" -name '*.json' | wc -l | tr -d ' ') vector files in $DEST"
