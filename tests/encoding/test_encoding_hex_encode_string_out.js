// covers: integration c:src/modules/encoding/encoding-hex.c c:src/modules/encoding/encoding-base64.c c:src/modules/encoding/encoding-base64url.c c:src/modules/encoding/encoding-base32.c c:src/modules/encoding/encoding-base85.c c:src/core/dyn-codec.c c:src/runtime/class.inc.c -- every text encoder writes into the result string allocated by JS_NewString8Buf in class.inc.c
// The text encoders allocate the result string first (JS_NewString8Buf, exact
// length: hex 2n, base64 4*ceil(n/3), base64url unpadded, base32 8*ceil(n/5),
// base85 from a count of all-zero groups) and encode straight into it, instead
// of malloc + encode + UTF-8 scan + copy + free. Base64URL encode maps +/ to -_
// with a branch-free pass over full groups plus a 2-3 char tail; decode maps
// back branch-free and rejects + and / afterwards. Base85 decode takes five
// in-range chars at a time when no whitespace or 'z' interrupts a group.
// Modperf round 2026-11-15, 400 KB: hex enc+dec -35%, base64 x2 -58%, base32
// -9%, base85 -28%. Oracle: straightforward JS reference codecs.
import { suite } from "../lib/kit.js";
import * as e from "dyna:encoding";

const t = suite("test_encoding_hex_encode_string_out");

const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const B32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567", B32H = "0123456789ABCDEFGHIJKLMNOPQRSTUV";
const refHex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");
function refB64(b, url) {
    let s = "";
    for (let i = 0; i < b.length; i += 3) {
        const n = Math.min(3, b.length - i);
        const v = (b[i] << 16) | ((n > 1 ? b[i + 1] : 0) << 8) | (n > 2 ? b[i + 2] : 0);
        for (let k = 0; k < 4; k++) s += k <= n ? B64[(v >> (18 - 6 * k)) & 63] : url ? "" : "=";
    }
    return url ? s.replace(/\+/g, "-").replace(/\//g, "_") : s;
}
function refB32(b, A) {
    let s = "";
    const nc = [0, 2, 4, 5, 7, 8];
    for (let i = 0; i < b.length; i += 5) {
        const n = Math.min(5, b.length - i);
        let v = 0n;
        for (let k = 0; k < 5; k++) v = (v << 8n) | BigInt(k < n ? b[i + k] : 0);
        for (let k = 0; k < 8; k++) s += k < nc[n] ? A[Number((v >> BigInt(35 - 5 * k)) & 31n)] : "=";
    }
    return s;
}
function refB85(b) {
    let s = "";
    for (let i = 0; i < b.length; i += 4) {
        const n = Math.min(4, b.length - i);
        let v = 0;
        for (let k = 0; k < 4; k++) v = v * 256 + (k < n ? b[i + k] : 0);
        if (n === 4 && v === 0) {
            s += "z";
            continue;
        }
        const c = [];
        for (let k = 0; k < 5; k++) {
            c.unshift(String.fromCharCode(33 + (v % 85)));
            v = Math.floor(v / 85);
        }
        s += c.slice(0, n + 1).join("");
    }
    return s;
}
function refB85Decode(s) {
    const out = [];
    let g = [];
    for (const ch of s) {
        if (" \t\n\v\f\r".includes(ch)) continue;
        if (ch === "z" && g.length === 0) {
            out.push(0, 0, 0, 0);
            continue;
        }
        const d = ch.charCodeAt(0) - 33;
        if (d < 0 || d > 84) return "bad";
        g.push(d);
        if (g.length === 5) {
            const v = g.reduce((a, x) => a * 85 + x, 0);
            if (v > 0xffffffff) return "bad";
            out.push((v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255);
            g = [];
        }
    }
    if (g.length === 1) return "bad";
    if (g.length) {
        const k = g.length;
        while (g.length < 5) g.push(84);
        const v = g.reduce((a, x) => a * 85 + x, 0);
        if (v > 0xffffffff) return "bad";
        const w = [(v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255];
        out.push(...w.slice(0, k - 1));
    }
    return out.join(",");
}

let seed = 0x9e3779b9;
const rnd = () => ((seed ^= seed << 13), (seed ^= seed >>> 17), (seed ^= seed << 5), seed >>> 0);
function bytes(n, kind) {
    const b = new Uint8Array(n);
    for (let i = 0; i < n; i++) b[i] = kind === "zeros" ? (i % 9 < 5 ? 0 : rnd() & 255) : kind === "high" ? 0xf8 | (rnd() & 7) : rnd() & 255;
    return b;
}
const roundTrip = (s) => { try { return Array.from(s).join(","); } catch (x) { return "E"; } };

t.test("encoders equal the JS reference at every length 0..260", ({ eq }) => {
    for (let n = 0; n <= 260; n++)
        for (const kind of ["rand", "zeros", "high"]) {
            const b = bytes(n, kind);
            eq(e.HexEncode(b), refHex(b), "hex n=" + n + " " + kind);
            eq(e.Base64Encode(b), refB64(b, false), "b64 n=" + n + " " + kind);
            eq(e.Base64URLEncode(b), refB64(b, true), "b64url n=" + n + " " + kind);
            eq(e.Base32Encode(b), refB32(b, B32), "b32 n=" + n + " " + kind);
            eq(e.Base32HexEncode(b), refB32(b, B32H), "b32hex n=" + n + " " + kind);
            eq(e.Base85Encode(b), refB85(b), "b85 n=" + n + " " + kind);
        }
});

t.test("decoders invert the encoders; base64url rejects + and /", ({ eq, throws }) => {
    for (let n = 0; n <= 130; n++) {
        const b = bytes(n, n & 1 ? "rand" : "zeros"), want = Array.from(b).join(",");
        eq(roundTrip(e.HexDecode(refHex(b))), want, "hex n=" + n);
        eq(roundTrip(e.Base64URLDecode(refB64(b, true))), want, "b64url n=" + n);
        eq(roundTrip(e.Base85Decode(refB85(b))), want, "b85 n=" + n);
        const u = refB64(b, true);
        if (u.length > 1) {
            throws(() => e.Base64URLDecode("+" + u.slice(1)), undefined, "b64url + n=" + n);
            throws(() => e.Base64URLDecode(u.slice(0, -1) + "/"), undefined, "b64url / n=" + n);
        }
    }
});

t.test("base85 decode with whitespace, z groups and bad digits matches the reference", ({ eq }) => {
    const pieces = ["!!!!!", "s8W-!", "z", " ", "\n", "uuuuu", "s8W-\"", "v", "!!", "@", "9jqo^", "\t"];
    for (let c = 0; c < 3000; c++) {
        let s = "";
        const k = 1 + (rnd() % 8);
        for (let i = 0; i < k; i++) s += pieces[rnd() % pieces.length];
        let got;
        try {
            got = Array.from(e.Base85Decode(s)).join(",");
        } catch (x) {
            got = "bad";
        }
        eq(got, refB85Decode(s), "case " + JSON.stringify(s));
    }
});

await t.run();
