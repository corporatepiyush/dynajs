// flags: --std
// Split from tests/test_audit_w4_native_1.js: the core sections.
import "../lib/httpc.js";
import { suite } from "../lib/kit.js";

const t = suite("test_core_audit_w4_native_1");
const per = (f, n = 20000) => { let b = Infinity; for (let r = 0; r < 5; r++) { const t0 = performance.now(); for (let i = 0; i < n; i++) f(); b = Math.min(b, (performance.now() - t0) / n * 1e6); } return b; };
// ORACLE: the WHATWG Encoding Standard's UTF-8 decoder is a function of the
// byte SEQUENCE: feeding it one byte at a time with {stream: true} yields the
// same text as one call. Valid input now skips the byte-wise state machine
// after one vectorised validation, so the two paths are different code and
// must still agree -- on valid text, on every malformed form (lone
// continuation, overlong, surrogate, beyond U+10FFFF, truncated tail), and
// with those forms placed across the 32-byte gate.
// SCALING: 4 MiB of ASCII decodes at memory speed, not at 430 MB/s.
// CONTROL: an input that is invalid only at its tail takes the old path at
// the old speed and still produces U+FFFD.
t.test("TextDecoder: whole valid input and byte-wise streaming agree", ({ ok, eq, best }) => {
    const whole = (u, fatal) => new TextDecoder("utf-8", { fatal }).decode(u);
    const drip = (u, fatal) => { const d = new TextDecoder("utf-8", { fatal }); let s = ""; for (let i = 0; i < u.length; i++) s += d.decode(u.subarray(i, i + 1), { stream: true }); return s + d.decode(); };
    const pad = (n) => new Array(n).fill(0x61);
    const forms = {
        "ascii": [0x68, 0x69],
        "2-byte": [0xc3, 0xa9], "3-byte": [0xe2, 0x9c, 0x93], "4-byte": [0xf0, 0x9f, 0x98, 0x80],
        "BOM": [0xef, 0xbb, 0xbf],
        "lone continuation": [0x80], "overlong 2": [0xc0, 0xaf], "overlong 3": [0xe0, 0x80, 0xaf],
        "surrogate": [0xed, 0xa0, 0x80], "beyond U+10FFFF": [0xf4, 0x90, 0x80, 0x80], "invalid lead": [0xff],
        "truncated 3-byte": [0xe2, 0x9c], "truncated 4-byte": [0xf0, 0x9f],
    };
    let bad = 0, n = 0;
    for (const [name, f] of Object.entries(forms)) {
        for (const before of [0, 1, 29, 30, 31, 32, 33, 64]) {
            for (const after of [0, 1, 31, 32, 40]) {
                const u = new Uint8Array([...pad(before), ...f, ...pad(after)]);
                n++;
                if (whole(u, false) !== drip(u, false)) { bad++; if (bad < 4) console.log("  differs: " + name + " before=" + before + " after=" + after); }
                let w, d;
                try { w = whole(u, true); } catch (e) { w = "THROW"; }
                try { d = drip(u, true); } catch (e) { d = "THROW"; }
                n++;
                if (w !== d) { bad++; if (bad < 4) console.log("  fatal differs: " + name + " before=" + before + " after=" + after); }
            }
        }
    }
    eq(bad, 0, "whole and byte-wise decoding agree on " + n + " inputs");
    eq(whole(new Uint8Array([...pad(40), 0xe2, 0x9c, 0x93]), false), "a".repeat(40) + "✓", "a 3-byte sequence after 40 ASCII bytes");
    eq(whole(new Uint8Array([0xef, 0xbb, 0xbf, ...pad(40)]), false), "a".repeat(40), "a leading BOM is stripped on the whole-input path");
    eq(whole(new Uint8Array([...pad(40), 0xff]), false), "a".repeat(40) + "�", "control: an invalid tail still becomes U+FFFD");

    const big = new TextEncoder().encode("plain ascii prose with few specials and no surprises at all ".repeat(70000));
    const tailBad = big.slice(); tailBad[tailBad.length - 3] = 0xff;
    const d = new TextDecoder();
    ok(d.decode(big).length === big.length, "4 MiB of ASCII decodes to as many characters");
    const tGood = best(() => d.decode(big)), tBad = best(() => d.decode(tailBad));
    // measured 17-21x alone; 2x leaves room for a host sharing its memory bandwidth
    ok(tGood * 2 < tBad, "[red] valid input skips the byte-wise decoder (" + tGood.toFixed(2) + " ms vs " + tBad.toFixed(2) + " ms for the same bytes with an invalid tail)");
});
await t.run();
