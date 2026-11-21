// covers: c:src/modules/validate/validate-formats.c
// IsEmail classifies each byte with one 256-entry table (bit 1: allowed in the local part, bit 2: in
// a domain label, bit 4: in the top-level label) and finds the '@' with memchr, instead of three
// helper calls per byte and a byte loop for '@': 10-17% faster on typical addresses. The oracle is
// a JS port of the previous byte-by-byte rules. It is run on every printable ASCII byte and a few
// control and non-ASCII bytes in each of the three positions, plus random addresses and every length
// limit (3/254 total, 64 local, 3/253 domain, 63 per label, 2-letter TLD).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_email_classes");
import * as V from "dyna:validate";

let pass = 0, fail = 0;
const eq = (a, b, m) => { if (a === b) pass++; else { fail++; print("FAIL " + m + ": got " + a + ", want " + b); } };
const ATEXT = "!#$%&'*+-/=?^_`{|}~";
const alpha = (c) => (c >= "a" && c <= "z") || (c >= "A" && c <= "Z");
const digit = (c) => c >= "0" && c <= "9";
function oracle(s) {
    for (const ch of s) if (ch.charCodeAt(0) > 127) return false;
    const n = s.length;
    if (n < 3 || n > 254) return false;
    const at = s.indexOf("@");
    if (at < 0 || s.indexOf("@", at + 1) >= 0) return false;
    if (at === 0 || at + 1 >= n || at > 64) return false;
    if (s[0] === "." || s[at - 1] === ".") return false;
    for (let i = 0; i < at; i++) {
        const c = s[i];
        if (c === ".") { if (i + 1 < at && s[i + 1] === ".") return false; continue; }
        if (!alpha(c) && !digit(c) && !ATEXT.includes(c)) return false;
    }
    const dn = n - at - 1;
    if (dn < 3 || dn > 253) return false;
    if (s[at + 1] === "." || s[at + 1] === "-" || s[n - 1] === "." || s[n - 1] === "-") return false;
    let lab = 0, dot = -1;
    for (let i = at + 1; i < n; i++) {
        const c = s[i];
        if (c === ".") {
            if (i + 1 < n && s[i + 1] === ".") return false;
            if (i > at + 1 && s[i - 1] === "-") return false;
            if (lab > 63) return false;
            lab = 0; dot = i; continue;
        }
        lab++;
        if (!alpha(c) && !digit(c) && c !== "-") return false;
        if (c === "-" && s[i - 1] === ".") return false;
    }
    if (lab > 63) return false;
    if (dot < 0 || n - dot - 1 < 2) return false;
    for (let i = dot + 1; i < n; i++) if (!alpha(s[i])) return false;
    return true;
}
const chk = (s, m) => eq(V.IsEmail(s), oracle(s), (m || "") + " " + JSON.stringify(s));
const bytes = [];
for (let c = 32; c < 127; c++) bytes.push(String.fromCharCode(c));
bytes.push("\t", "\n", "\x00", "\x7f", "é", "ÿ", "Ā", "€");

__kitS.section("each byte in the local part", () => {
    for (const b of bytes) {
        chk("a" + b + "b@example.com", "local middle");
        chk(b + "x@example.com", "local first");
        chk("x" + b + "@example.com", "local last");
    }
    eq(V.IsEmail("!#$%&'*+-/=?^_`{|}~@example.com"), true, "all atext specials");
    eq(V.IsEmail("Az09@example.com"), true, "letters and digits");
});

__kitS.section("each byte in a domain label and in the TLD", () => {
    for (const b of bytes) {
        chk("user@ex" + b + "ample.com", "label middle");
        chk("user@" + b + "example.com", "label first");
        chk("user@example" + b + ".com", "label last");
        chk("user@example.c" + b + "m", "tld middle");
        chk("user@example.co" + b, "tld last");
        chk("user@a.b" + b + "c.org", "inner label");
    }
    eq(V.IsEmail("u@a-b.c-d.example.com"), true, "hyphens inside labels");
    eq(V.IsEmail("u@123.example.com"), true, "digit label");
    eq(V.IsEmail("u@example.c0m"), false, "digit in the TLD");
    eq(V.IsEmail("u@example.c-m"), false, "hyphen in the TLD");
});

__kitS.section("the at sign", () => {
    for (const s of ["a@b@example.com", "@example.com", "user@", "user", "a@@example.com", "user@example.com@", "@@@", "a@b.cd", "a@bc", "ab@c.de"]) chk(s, "at");
    eq(V.IsEmail("a@b.cd"), true, "shortest domain with a dot");
});

__kitS.section("length limits", () => {
    const l = (k) => "a".repeat(k);
    for (const k of [63, 64, 65]) chk(l(k) + "@example.com", "local length " + k);
    for (const k of [62, 63, 64]) chk("u@" + l(k) + ".com", "label length " + k);
    for (const k of [62, 63, 64]) chk("u@example." + l(k), "tld length " + k);
    const dom = (k) => { let d = ""; while (d.length + 4 <= k) d += "abc."; return (d + "com").slice(-k).replace(/^\./, "x"); };
    for (const k of [252, 253, 254]) chk("u@" + dom(k), "domain length " + k);
    for (const k of [253, 254, 255]) chk(l(64) + "@" + dom(k - 65), "total length " + k);
    chk("a@b", "three bytes");
    chk("a@", "two bytes");
});

__kitS.section("random addresses agree with the oracle", () => {
    let x = 0x9e3779b9;
    const rnd = (k) => { x ^= x << 13; x ^= x >>> 17; x ^= x << 5; return (x >>> 0) % k; };
    const pool = "abcxyzABZ0189.-_+@!~`{}|/=?^#$%&'*é ";
    for (let i = 0; i < 20000; i++) {
        let s = "";
        const len = 1 + rnd(30);
        for (let j = 0; j < len; j++) s += pool[rnd(pool.length)];
        if (i & 1) s = s.replace(/@/g, "") + "@" + ["example.com", "a-b.org", "x.y.z.io", "q.c", "e..com", "-e.com", "e-.com", "e.c1"][rnd(8)];
        chk(s, "random");
    }
});

__kitS.end();
print(`test_validate_email_classes: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
