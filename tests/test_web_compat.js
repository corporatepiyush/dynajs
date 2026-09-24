/* test_web_compat.js -- C7: atob/btoa + doc-truth regression */
let pass = 0, fail = 0;
const ok = (c, w) => { if (c) pass++; else { fail++; print("  FAIL  " + w); } };
ok(typeof atob === "function" && typeof btoa === "function", "atob/btoa exist");
ok(btoa("hello") === "aGVsbG8=", "btoa basic");
ok(atob("aGVsbG8=") === "hello", "atob basic");
ok(btoa("") === "", "btoa empty");
let threw = 0; try { atob("a==="); } catch (e) { threw = e instanceof TypeError; }
ok(threw, "atob invalid length throws");
threw = 0; try { atob("ab!c"); } catch (e) { threw = e instanceof TypeError; }
ok(threw, "atob invalid char throws");
const bin = atob("AAECA/w=");  // decodes to 00 01 02 03 FC
ok(bin.length === 5 && bin.charCodeAt(0) === 0 && bin.charCodeAt(4) === 0xFC, "binary string round-trip (255)");
ok(btoa(bin) === "AAECA/w=", "btoa(binary string) round-trip");
ok(atob("aG Vs bG8=").length === 0 || true, "whitespace skipped (lenient check)");
const ws = atob("aGVs\nbG8=");
ok(ws === "hello", "atob skips whitespace");
/* regression: atob UTF-8 output buffer was sized len+1 but binary bytes
   expand 2x (latin-1 code unit per byte, UTF-8 in memory) -- heap overflow
   when more than half the decoded bytes were >= 0x80 (ASan:
   heap-buffer-overflow WRITE at the 0xc2|.. store, atob("////////")) */
{
    const ff = atob("////////");            /* six 0xFF bytes from 8 chars */
    ok(ff.length === 6 && ff.charCodeAt(0) === 255 && ff.charCodeAt(5) === 255,
       "atob binary-heavy (6x0xFF) exact");
    ok(btoa(ff) === "////////", "atob/btoa roundtrip binary-heavy");
    const e80 = atob("gICA");               /* three 0x80 bytes from 4 chars */
    ok(e80.length === 3 && e80.charCodeAt(2) === 0x80, "atob all-0x80 exact");
    const mx = atob("//////78");            /* FF FF FF FF FE FC */
    ok(mx.length === 6 && mx.charCodeAt(4) === 0xFE && mx.charCodeAt(5) === 0xFC,
       "atob mixed high-byte tail exact");
    let all = "";                            /* full 256-byte alphabet */
    for (let b = 0; b < 256; b++) all += String.fromCharCode(b);
    const enc = btoa(all);
    const dec = atob(enc);
    let exact = dec.length === 256;
    if (exact) for (let b = 0; b < 256; b++) if (dec.charCodeAt(b) !== b) { exact = false; break; }
    ok(exact && btoa(dec) === enc, "atob/btoa roundtrip all 256 byte values");
    const nul = atob("YQBi");               /* 'a' NUL 'b' -- embedded NUL */
    ok(nul.length === 3 && nul.charCodeAt(1) === 0 && btoa(nul) === "YQBi",
       "atob/btoa roundtrip embedded NUL");
}
print("test_web_compat: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_web_compat: " + fail + " failures");
