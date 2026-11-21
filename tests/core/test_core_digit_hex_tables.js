// covers: c:src/cutils.h
// covers: c:src/engine-internal.h
// covers: c:src/dtoa.c
// to_digit (engine-internal.h, dtoa.c) and from_hex (cutils.h) are 256-entry
// lookup tables; every byte, and code points past 255 that must not alias a
// digit through a truncating index, is checked against a plain JS oracle that
// never touches those tables.
import { suite } from "../lib/kit.js";

const t = suite("test_core_digit_hex_tables");
const ALPHA = "0123456789abcdefghijklmnopqrstuvwxyz";
const digit36 = (ch) => {
    const k = ch.length === 1 ? ALPHA.indexOf(ch.toLowerCase()) : -1;
    return ch.length === 1 && ch.charCodeAt(0) < 128 && k >= 0 ? k : 36;
};
const hexv = (ch) => {
    const k = digit36(ch);
    return k < 16 ? k : -1;
};
const CHARS = [];
for (let c = 0; c < 256; c++) CHARS.push(String.fromCharCode(c));
for (const c of [0x130, 0x131, 0x17f, 0x212a, 0x0130 + 0x30 - 0x30, 0x0161, 0x0661, 0xff10, 0xff21, 0xff41, 0x100 + 0x30, 0x100 + 0x41, 0x100 + 0x61, 0x200 + 0x39])
    CHARS.push(String.fromCharCode(c));

t.test("parseInt accepts exactly the digits below the radix", ({ eq }) => {
    let bad = 0;
    for (let radix = 2; radix <= 36; radix++) {
        for (const ch of CHARS) {
            const d = digit36(ch);
            const want = d < radix ? radix + d : 1;
            const got = parseInt("1" + ch + "☃", radix);
            if (got !== want && bad++ < 5) eq(got, want, `parseInt("1"+U+${ch.charCodeAt(0).toString(16)}, ${radix})`);
        }
    }
    eq(bad, 0, "mismatches");
});

t.test("Number() radix literals use the same digit values", ({ eq }) => {
    let bad = 0;
    const pre = { 2: "0b", 8: "0o", 16: "0x" };
    for (const radix of [2, 8, 16]) {
        for (const ch of CHARS) {
            const d = digit36(ch);
            const want = d < radix ? (radix + d) * radix : NaN;
            const got = Number(pre[radix] + "1" + ch + "0");
            if (!Object.is(got, want) && bad++ < 5) eq(got, want, `Number(${pre[radix]}1 U+${ch.charCodeAt(0).toString(16)} 0)`);
        }
    }
    eq(bad, 0, "mismatches");
});

t.test("BigInt() hex strings use the same digit values", ({ eq }) => {
    let bad = 0;
    for (const ch of CHARS) {
        const d = hexv(ch);
        let got;
        try {
            got = BigInt("0x1" + ch);
        } catch (e) {
            got = e instanceof SyntaxError ? "SyntaxError" : String(e);
        }
        const want = d >= 0 ? BigInt(16 + d) : (ch.trim() === "" && ch !== "\0" ? 1n : "SyntaxError");
        if (got !== want && bad++ < 5) eq(got, want, `BigInt("0x1"+U+${ch.charCodeAt(0).toString(16)})`);
    }
    eq(bad, 0, "mismatches");
});

t.test("Uint8Array.fromHex decodes every hex pair and refuses every other byte", ({ eq }) => {
    let bad = 0;
    for (const a of CHARS) {
        for (const b of ["0", "F", "a", "9", "g", "G", "/", ":", "@", "`"]) {
            const ha = hexv(a), hb = hexv(b);
            let got;
            try {
                got = Uint8Array.fromHex(a + b)[0];
            } catch (e) {
                got = e instanceof SyntaxError ? "SyntaxError" : String(e);
            }
            const want = ha >= 0 && hb >= 0 ? ha * 16 + hb : "SyntaxError";
            if (got !== want && bad++ < 5) eq(got, want, `fromHex(U+${a.charCodeAt(0).toString(16)} ${b})`);
        }
    }
    eq(bad, 0, "mismatches");
});

t.test("fromHex of a long mixed-case string matches a per-pair oracle", ({ eq }) => {
    const H = "0123456789abcdefABCDEF";
    let s = "", seed = 7;
    for (let i = 0; i < 4096; i++) {
        seed = (Math.imul(seed, 1103515245) + 12345) >>> 0;
        s += H[(seed >>> 8) % 22];
    }
    const got = Uint8Array.fromHex(s);
    let bad = 0;
    for (let i = 0; i < got.length; i++)
        if (got[i] !== hexv(s[2 * i]) * 16 + hexv(s[2 * i + 1])) bad++;
    eq(got.length, 2048, "length");
    eq(bad, 0, "mismatched bytes");
});

t.test("decodeURIComponent and \\x / \\u escapes use the same hex values", ({ eq }) => {
    let bad = 0;
    for (const ch of CHARS) {
        const d = hexv(ch);
        let got;
        try {
            got = decodeURIComponent("%4" + ch);
        } catch (e) {
            got = e instanceof URIError ? "URIError" : String(e);
        }
        const want = d >= 0 ? String.fromCharCode(0x40 + d) : "URIError";
        if (got !== want && bad++ < 5) eq(got, want, `decodeURIComponent(%4 U+${ch.charCodeAt(0).toString(16)})`);
        if (ch === "\\" || ch === "\"" || ch === "\n" || ch === "\r" || ch === " " || ch === " ") continue;
        let lit;
        try {
            lit = (0, eval)("\"\\x4" + ch + "\"");
        } catch (e) {
            lit = e instanceof SyntaxError ? "SyntaxError" : String(e);
        }
        if (lit !== want.replace("URIError", "SyntaxError") && bad++ < 5) eq(lit, want, `"\\x4" U+${ch.charCodeAt(0).toString(16)}`);
        let re;
        try {
            re = new RegExp("^\\u004" + ch + "$", "u").test(String.fromCharCode(0x40 + d));
        } catch (e) {
            re = e instanceof SyntaxError ? "SyntaxError" : String(e);
        }
        if (re !== (d >= 0 ? true : "SyntaxError") && bad++ < 5) eq(re, d >= 0, `/\\u004 U+${ch.charCodeAt(0).toString(16)}/u`);
    }
    eq(bad, 0, "mismatches");
});

await t.run();
