// covers: c:src/runtime/class.inc.c
// tests/core/test_tocstring_exact.js -- COMPAT-6 (B16): converting a UTF-16
// string to UTF-8 for a native call used to reserve 3 bytes per unit and to
// refuse any string longer than 2^30/3 units, whatever its UTF-8 length. When
// the 3-per-unit reservation would pass 1 MiB it now counts first (1 byte per
// unit, +1 at U+0080, +1 at U+0800; a surrogate pair is reserved 6 and written
// 4), so a mostly-ASCII wide string costs about one byte per unit. Runs of
// ASCII are copied 8 units at a time. The byte image must match a reference
// WTF-8 encoder exactly: pairs are 4 bytes, a lone surrogate 3 bytes, 2- and
// 3-byte BMP characters as usual.
// build-note: needs dyna:sys and dyna:hash (build with CONFIG_NATIVE_MODULES=y)
import { sections as __kitSections } from "../lib/kit.js";
import { SHA256Hex } from "dyna:hash";
import { Exec } from "dyna:sys";
const __kitS = __kitSections("test_tocstring_exact");

const BIN = scriptArgs[1] || "./dynajs";
let n = 0, failures = 0;
function check(cond, msg) { n++; if (!cond) { failures++; print("  FAIL:", msg); } }

function wtf8(s) {
    const out = [];
    for (let i = 0; i < s.length; i++) {
        let c = s.charCodeAt(i);
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
            const d = s.charCodeAt(i + 1);
            if (d >= 0xdc00 && d <= 0xdfff) { c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00); i++; }
        }
        if (c < 0x80) out.push(c);
        else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
        else if (c < 0x10000) out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
        else out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
    }
    return new Uint8Array(out);
}

const PIECES = ["a", "\u00e9", "\u4e2d", "\u{1F600}", "\ud800", "\udc00", "x\ud83d", "\ude00y", "\u07ff", "\u0800", "\uffff"];
__kitS.section("an ASCII run with ragged wide tails is copied exactly", () => {
for (const [label, len] of [["short", 21000], ["just under the 1 MiB reservation", 349000], ["past it", 400000], ["far past", 1500000]]) {
    let s = "";
    let k = 0;
    while (s.length < len) s += PIECES[k++ % PIECES.length];
    s += "\ud800";
    const ascii = "x".repeat(len) + "\u00e9" + "abcdefgh".repeat(9) + "\u4e2d" + "abcdefg";
    check(SHA256Hex(ascii) === SHA256Hex(wtf8(ascii)), label + ": an ASCII run with ragged wide tails is copied exactly");
    check(SHA256Hex(s) === SHA256Hex(wtf8(s)), label + ": the native sees the exact WTF-8 bytes (" + s.length + " units)");
}
});

__kitS.end();
if (!__kitS.listing) {

print("test_tocstring_exact: " + n + " checks, " + failures + " failures");
if (failures) throw new Error("test_tocstring_exact: " + failures + " failures");
}
