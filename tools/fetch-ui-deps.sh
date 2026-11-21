#!/usr/bin/env bash
# Fetch the third-party inputs of dyna:ui into third_party/ui/ (gitignored): stb_truetype,
# stb_rect_pack and stb_textedit (public domain / MIT), the ProggyClean font (MIT), Inter (OFL 1.1,
# the scalable default at scales other than 1x; a variable TTF whose default instance stb renders), and microui
# (MIT, kept for reference -- src/core/dyn-ui.c is the rewrite). Every file is pinned to a commit
# and checked against its SHA-256; a mismatch deletes the file and fails. Files already present
# with the right hash are not downloaded again. ProggyClean is also emitted as a C byte array
# (proggy_clean.inc) for the build to embed.
#
#   tools/fetch-ui-deps.sh          fetch what is missing
#   tools/fetch-ui-deps.sh --check  only verify; exit 1 if anything is missing or wrong
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/third_party/ui"
STB=003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5
IMGUI=f88ec043afbb800c99d05ad74e348bd70e10da9d
MU=0850aba860959c3e75fb3e97120ca92957f9d057
GF=6157dfaa640ca5738b4d1a7070fe389c3036d7fc

# destination | url | sha256
FILES="
stb/stb_truetype.h|https://raw.githubusercontent.com/nothings/stb/$STB/stb_truetype.h|ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab
stb/stb_rect_pack.h|https://raw.githubusercontent.com/nothings/stb/$STB/stb_rect_pack.h|6388366a30de57c1b4c29b6eed4ed1c7d758fc1443d2dd95cfce943370bb3aa1
stb/stb_textedit.h|https://raw.githubusercontent.com/nothings/stb/$STB/stb_textedit.h|7af0ded9e07ee2dcd1dab9e1413f59ac66d0a4f7bdaa39b4aaaca08cf691d0f3
stb/LICENSE|https://raw.githubusercontent.com/nothings/stb/$STB/LICENSE|bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63
fonts/ProggyClean.ttf|https://raw.githubusercontent.com/ocornut/imgui/$IMGUI/misc/fonts/ProggyClean.ttf|527d2a443ce051f93f7e77b855609722b8cb220a9f104b4aa037be5c90b71324
microui/microui.c|https://raw.githubusercontent.com/rxi/microui/$MU/src/microui.c|880ce7e017fe307553586c9ba8291e9d9f56cdb20bc4188df072f1e8bd9af6dd
microui/microui.h|https://raw.githubusercontent.com/rxi/microui/$MU/src/microui.h|9aa08e7f58c2152dbfcb4d7deb1f5d79ab1bf0c2809d87f3ff89dc34e24d0c63
fonts/Inter.ttf|https://raw.githubusercontent.com/google/fonts/$GF/ofl/inter/Inter%5Bopsz,wght%5D.ttf|29160a80ff49ddcab2c97711247e08b1fab27a484a329ce8b813d820dc559031
fonts/Inter-OFL.txt|https://raw.githubusercontent.com/google/fonts/$GF/ofl/inter/OFL.txt|5b9321a4298cfeb6b34354164a1c3afc3db114569984c502b9b35d988fd58c57
microui/LICENSE|https://raw.githubusercontent.com/rxi/microui/$MU/LICENSE|470601a025efe1d098dcce9b86b4e5b3fbdb9341c1f9ec285cf9435f6caa0a38
"

sha() { shasum -a 256 "$1" 2>/dev/null | cut -d' ' -f1 || sha256sum "$1" | cut -d' ' -f1; }

check_only=0
[ "${1:-}" = "--check" ] && check_only=1
bad=0
while IFS='|' read -r rel url want; do
  [ -n "$rel" ] || continue
  f="$DEST/$rel"
  if [ -f "$f" ] && [ "$(sha "$f")" = "$want" ]; then continue; fi
  if [ "$check_only" = 1 ]; then echo "fetch-ui-deps: missing or wrong: third_party/ui/$rel"; bad=1; continue; fi
  mkdir -p "$(dirname "$f")"
  echo "fetch-ui-deps: $rel"
  curl -fsSL --retry 2 -o "$f.part" "$url" || { rm -f "$f.part"; echo "fetch-ui-deps: download failed: $url" >&2; exit 1; }
  got="$(sha "$f.part")"
  if [ "$got" != "$want" ]; then
    rm -f "$f.part"
    echo "fetch-ui-deps: $rel has sha256 $got, expected $want -- refusing it" >&2
    exit 1
  fi
  mv "$f.part" "$f"
done <<EOF
$FILES
EOF
[ "$bad" = 0 ] || exit 1

emit_inc() {
  local src="$1" inc="$2"
  if [ "$check_only" = 0 ] && { [ ! -f "$inc" ] || [ "$src" -nt "$inc" ]; }; then
    od -An -v -tu1 "$src" | tr -s ' \n' ' ' | sed -E 's/^ //; s/ $//; s/ /, /g; s/$/,/' | fold -w 100 -s > "$inc.part"
    mv "$inc.part" "$inc"
  fi
  [ -f "$inc" ] || { echo "fetch-ui-deps: missing ${inc#"$ROOT"/}" >&2; exit 1; }
}
emit_inc "$DEST/fonts/ProggyClean.ttf" "$DEST/fonts/proggy_clean.inc"
emit_inc "$DEST/fonts/Inter.ttf" "$DEST/fonts/inter.inc"
echo "fetch-ui-deps: third_party/ui is complete and verified"
