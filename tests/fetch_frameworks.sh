#!/bin/bash
# fetch_frameworks.sh -- download real-world JavaScript for the parser benchmark.
#
# The corpus is deliberately REAL shipped code, minified and not, from both
# browser and server/tooling projects. Generated or hand-written sources do not
# reproduce what a parser actually meets: a minified bundle is one enormous
# expression with 1-2 character identifiers and almost no whitespace, while the
# same library unminified is 10x the bytes with comments and long names. Those
# two exercise completely different parts of the lexer, and a change that helps
# one can cost the other.
#
# Files land in bench/frameworks/ (gitignored -- do not commit ~150 MB).
# Every entry is a UMD/global/CommonJS build, never an ES module, because the
# benchmark parses each file as a function body (see bench_parse_frameworks.js)
# and `import`/`export` are illegal there.
#
# Usage: bench/fetch_frameworks.sh [--force]

set -u
cd "$(dirname "$0")/.." || exit 1
OUT=bench/frameworks
mkdir -p "$OUT"
FORCE=${1:-}

# name|kind|url
MANIFEST=$(cat <<'EOF'
react-dom.dev|client|https://cdn.jsdelivr.net/npm/react-dom@18.3.1/umd/react-dom.development.js
react-dom.min|client|https://cdn.jsdelivr.net/npm/react-dom@18.3.1/umd/react-dom.production.min.js
vue.dev|client|https://cdn.jsdelivr.net/npm/vue@3.4.21/dist/vue.global.js
vue.min|client|https://cdn.jsdelivr.net/npm/vue@3.4.21/dist/vue.runtime.global.prod.js
angular.dev|client|https://cdn.jsdelivr.net/npm/angular@1.8.3/angular.js
angular.min|client|https://cdn.jsdelivr.net/npm/angular@1.8.3/angular.min.js
jquery.dev|client|https://cdn.jsdelivr.net/npm/jquery@3.7.1/dist/jquery.js
jquery.min|client|https://cdn.jsdelivr.net/npm/jquery@3.7.1/dist/jquery.min.js
three.dev|client|https://cdn.jsdelivr.net/npm/three@0.137.5/build/three.js
three.min|client|https://cdn.jsdelivr.net/npm/three@0.137.5/build/three.min.js
d3.dev|client|https://cdn.jsdelivr.net/npm/d3@7.8.5/dist/d3.js
d3.min|client|https://cdn.jsdelivr.net/npm/d3@7.8.5/dist/d3.min.js
lodash.dev|client|https://cdn.jsdelivr.net/npm/lodash@4.17.21/lodash.js
lodash.min|client|https://cdn.jsdelivr.net/npm/lodash@4.17.21/lodash.min.js
moment.dev|client|https://cdn.jsdelivr.net/npm/moment@2.30.1/moment.js
moment.min|client|https://cdn.jsdelivr.net/npm/moment@2.30.1/min/moment.min.js
echarts.dev|client|https://cdn.jsdelivr.net/npm/echarts@5.5.0/dist/echarts.js
echarts.min|client|https://cdn.jsdelivr.net/npm/echarts@5.5.0/dist/echarts.min.js
bootstrap.dev|client|https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/js/bootstrap.bundle.js
bootstrap.min|client|https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/js/bootstrap.bundle.min.js
pixi.min|client|https://cdn.jsdelivr.net/npm/pixi.js@7.4.2/dist/pixi.min.js
chart.dev|client|https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.js
typescript.dev|server|https://cdn.jsdelivr.net/npm/typescript@5.4.5/lib/typescript.js
babel.dev|server|https://cdn.jsdelivr.net/npm/@babel/standalone@7.24.4/babel.js
babel.min|server|https://cdn.jsdelivr.net/npm/@babel/standalone@7.24.4/babel.min.js
prettier.dev|server|https://cdn.jsdelivr.net/npm/prettier@3.2.5/standalone.js
terser.dev|server|https://cdn.jsdelivr.net/npm/terser@5.30.3/dist/bundle.min.js
acorn.dev|server|https://cdn.jsdelivr.net/npm/acorn@8.11.3/dist/acorn.js
axios.dev|server|https://cdn.jsdelivr.net/npm/axios@1.6.8/dist/axios.js
axios.min|server|https://cdn.jsdelivr.net/npm/axios@1.6.8/dist/axios.min.js
socketio.dev|server|https://cdn.jsdelivr.net/npm/socket.io-client@4.7.5/dist/socket.io.js
socketio.min|server|https://cdn.jsdelivr.net/npm/socket.io-client@4.7.5/dist/socket.io.min.js
corejs.dev|server|https://cdn.jsdelivr.net/npm/core-js-bundle@3.37.0/index.js
corejs.min|server|https://cdn.jsdelivr.net/npm/core-js-bundle@3.37.0/minified.js
EOF
)

ok=0; fail=0; skip=0
while IFS='|' read -r name kind url; do
    [ -n "$name" ] || continue
    dest="$OUT/$name.js"
    if [ -s "$dest" ] && [ "$FORCE" != "--force" ]; then
        skip=$((skip + 1)); continue
    fi
    if curl -fsSL --max-time 120 -o "$dest.tmp" "$url"; then
        # A CDN 404 page is HTML, not JS, and would silently poison the corpus.
        if head -c 400 "$dest.tmp" | grep -qi '<!doctype html\|<html'; then
            echo "REJECT (html) $name"; rm -f "$dest.tmp"; fail=$((fail + 1)); continue
        fi
        mv "$dest.tmp" "$dest"
        printf '  %-18s %-7s %8d KB\n' "$name" "$kind" "$(( $(wc -c < "$dest") / 1024 ))"
        ok=$((ok + 1))
    else
        echo "FAIL $name  $url"; rm -f "$dest.tmp"; fail=$((fail + 1))
    fi
done <<< "$MANIFEST"

echo "---"
echo "downloaded=$ok  cached=$skip  failed=$fail"
echo "total corpus: $(( $(cat "$OUT"/*.js 2>/dev/null | wc -c) / 1024 )) KB in $(ls "$OUT"/*.js 2>/dev/null | wc -l | tr -d ' ') files"
[ "$fail" -eq 0 ]
