// covers: integration c:src/modules/netip/netip-internal.h c:src/modules/netip/netip-module.c c:src/modules/netip/netip-classify.c c:src/modules/netip/netip-parse.c c:src/core/netipk/netipk-format.c -- one in-place argument reader serves every dyna:netip address function
// dyna:netip address functions read a one-byte (Latin-1) string argument in
// place through JS_GetNarrowStringBytes instead of converting it to UTF-8 and
// freeing the copy; a two-byte string or a non-string still goes through
// JS_ToCStringLen. Any byte above 0x7F is invalid in an address either way,
// so results cannot differ. An IPv6 group is printed by its hex width
// instead of reversed through a scratch buffer. Modperf round 2026-11-16
// (median of 7, 1K/10K/100K): isValid -11%, Prefix.contains -10%,
// compareAddr -6.5%, classifiers -5%, contains -4.5%; canonical (IPv6)
// -8.6%. Prefix.contains answers false for an invalid address. Oracle: a JS parser
// for strict dotted-quad IPv4 and RFC 4291 IPv6 text (with an embedded IPv4
// tail and one "::"), RFC 5952 canonical output, CIDR prefix rules, and
// byte-wise address order (IPv4 before IPv6).
import { suite } from "../lib/kit.js";
import * as netipMod from "dyna:netip";

const t = suite("test_netip_narrow_args");

function p4(s) {
    const f = s.split(".");
    if (f.length !== 4) return null;
    const o = [];
    for (const x of f) {
        if (!/^[0-9]{1,3}$/.test(x) || (x.length > 1 && x[0] === "0") || +x > 255) return null;
        o.push(+x);
    }
    return o;
}
function p6(s) {
    const dc = s.split("::");
    if (dc.length > 2) return null;
    const side = (str, tail) => {
        if (str === "") return [];
        const g = str.split(":"), out = [];
        for (let i = 0; i < g.length; i++) {
            if (tail && i === g.length - 1 && g[i].includes(".")) {
                const v4 = p4(g[i]);
                if (!v4) return null;
                out.push((v4[0] << 8) | v4[1], (v4[2] << 8) | v4[3]);
            } else {
                if (!/^[0-9a-fA-F]{1,4}$/.test(g[i])) return null;
                out.push(parseInt(g[i], 16));
            }
        }
        return out;
    };
    const a = side(dc[0], dc.length === 1), b = dc.length === 2 ? side(dc[1], true) : [];
    if (!a || !b) return null;
    if (dc.length === 1) return a.length === 8 ? a : null;
    if (a.length + b.length > 7) return null;
    return a.concat(new Array(8 - a.length - b.length).fill(0), b);
}
function parse(s) {
    const m = /[.:]/.exec(s);
    if (!m) return null;
    if (m[0] === ":") {
        const g = p6(s);
        return g ? { is4: false, b: g.flatMap((x) => [x >> 8, x & 255]) } : null;
    }
    const v = p4(s);
    return v ? { is4: true, b: v } : null;
}
function canon(a) {
    if (a.is4) return a.b.join(".");
    const g = [];
    for (let i = 0; i < 16; i += 2) g.push((a.b[i] << 8) | a.b[i + 1]);
    if (g.slice(0, 5).every((x) => x === 0) && g[5] === 0xffff) return "::ffff:" + a.b.slice(12).join(".");
    let bs = -1, bl = 0;
    for (let i = 0; i < 8; ) {
        if (g[i] === 0) {
            let j = i;
            while (j < 8 && g[j] === 0) j++;
            if (j - i > bl) (bl = j - i), (bs = i);
            i = j;
        } else i++;
    }
    if (bl < 2) return g.map((x) => x.toString(16)).join(":");
    return g.slice(0, bs).map((x) => x.toString(16)).join(":") + "::" + g.slice(bs + bl).map((x) => x.toString(16)).join(":");
}
function prefix(s) {
    const i = s.lastIndexOf("/");
    if (i < 0) return null;
    const a = parse(s.slice(0, i)), bs = s.slice(i + 1);
    if (!a || !/^[0-9]{1,3}$/.test(bs) || (bs.length > 1 && bs[0] === "0")) return null;
    const bits = +bs;
    return bits > (a.is4 ? 32 : 128) ? null : { a, bits };
}
function inPrefix(p, ip) {
    if (p.a.is4 !== ip.is4) return false;
    for (let k = 0; k < p.bits; k++) if (((p.a.b[k >> 3] >> (7 - (k & 7))) & 1) !== ((ip.b[k >> 3] >> (7 - (k & 7))) & 1)) return false;
    return true;
}
function cmp(x, y) {
    if (x.is4 !== y.is4) return x.is4 ? -1 : 1;
    for (let i = 0; i < x.b.length; i++) if (x.b[i] !== y.b[i]) return x.b[i] < y.b[i] ? -1 : 1;
    return 0;
}
const tryv = (f) => {
    try {
        return f();
    } catch (e) {
        return e instanceof TypeError ? "TypeError" : "other";
    }
};

let seed = 0x2611;
const rnd = (n) => ((seed = (seed * 1103515245 + 12345) >>> 0) >>> 8) % n;
const PIECES = ["0", "1", "10", "255", "256", "01", "a", "ff", "FFFF", "10000", ".", ":", "::", "/", "/24", "/0", "/129", "é", "ÿ", "Ā", "名", "\u0000", " ", "%", "1.2.3.4", "::ffff:1.2.3.4"];
const gen = () => Array.from({ length: 1 + rnd(7) }, () => PIECES[rnd(PIECES.length)]).join("");
const VALID = ["0.0.0.0", "255.255.255.255", "10.1.2.3", "127.0.0.1", "192.168.1.1", "::", "::1", "1::", "2001:db8::1", "fe80::1:2", "ff02::1", "::ffff:10.0.0.1", "1:2:3:4:5:6:7:8", "1:0:0:2:0:0:0:3", "::1.2.3.4", "2001:DB8:0:0:8:800:200C:417A"];
const INPUTS = [];
for (const v of VALID) INPUTS.push(v, v + "é", "é" + v, v + "Ā", v.slice(0, -1) + "名", v + "\u0000", v.toUpperCase(), (" " + v).slice(1), ("x" + v + "x").slice(1, -1));
for (let r = 0; r < 1500; r++) INPUTS.push(gen());

t.test("isValid, canonical and parseAddr agree with the JS parser", ({ eq }) => {
    for (const s of INPUTS) {
        const a = parse(s), name = JSON.stringify(s);
        eq(netipMod.isValid(s), !!a, "isValid " + name);
        eq(tryv(() => netipMod.canonical(s)), a ? canon(a) : "TypeError", "canonical " + name);
        const pa = tryv(() => netipMod.parseAddr(s));
        eq(typeof pa === "string" ? pa : JSON.stringify([pa.is4, [...pa.bytes], pa.string]), a ? JSON.stringify([a.is4, a.b, canon(a)]) : "TypeError", "parseAddr " + name);
    }
});

t.test("prefixes: parsePrefix, masked, contains, Prefix", ({ eq }) => {
    const P = [];
    for (const v of VALID) for (const b of ["/0", "/8", "/24", "/32", "/33", "/64", "/128", "/129", "/024", "/", ""]) P.push(v + b, v + b + "é");
    for (let r = 0; r < 600; r++) P.push(gen());
    for (const s of P) {
        const p = prefix(s), name = JSON.stringify(s);
        const got = tryv(() => netipMod.parsePrefix(s));
        eq(typeof got === "string" ? got : "ok", p ? "ok" : "TypeError", "parsePrefix " + name);
        eq(tryv(() => typeof new netipMod.Prefix(s)), p ? "object" : "TypeError", "Prefix " + name);
        if (!p) continue;
        const pf = new netipMod.Prefix(s);
        for (let k = 0; k < 6; k++) {
            const ip = rnd(2) ? VALID[rnd(VALID.length)] : INPUTS[rnd(INPUTS.length)];
            const a = parse(ip);
            eq(tryv(() => netipMod.contains(s, ip)), a ? inPrefix(p, a) : "TypeError", "contains " + name + " " + JSON.stringify(ip));
            eq(tryv(() => pf.contains(ip)), a ? inPrefix(p, a) : false, "Prefix.contains " + name + " " + JSON.stringify(ip));
        }
    }
});

t.test("compareAddr orders like the byte-wise oracle", ({ eq }) => {
    for (let r = 0; r < 2000; r++) {
        const x = rnd(3) ? VALID[rnd(VALID.length)] : INPUTS[rnd(INPUTS.length)];
        const y = rnd(3) ? VALID[rnd(VALID.length)] : INPUTS[rnd(INPUTS.length)];
        const a = parse(x), b = parse(y);
        eq(tryv(() => netipMod.compareAddr(x, y)), a && b ? cmp(a, b) : "TypeError", "compare " + JSON.stringify([x, y]));
    }
});

t.test("classifiers on narrow, wide and non-string arguments", ({ eq }) => {
    for (const s of INPUTS.slice(0, 400)) {
        const a = parse(s);
        for (const f of ["isLoopback", "isPrivate", "isMulticast", "isUnspecified", "isLinkLocalUnicast", "isGlobalUnicast", "isLinkLocalMulticast"]) {
            const got = tryv(() => netipMod[f](s));
            eq(a ? typeof got : got, a ? "boolean" : "TypeError", f + " " + JSON.stringify(s));
        }
    }
    eq(netipMod.isLoopback("127.0.0.1"), true, "loopback");
    eq(netipMod.isPrivate("10.9.8.7"), true, "private");
    eq(netipMod.isLoopback("::1"), true, "v6 loopback");
    eq(netipMod.isValid(1234), false, "number is not valid");
    eq(tryv(() => new netipMod.Prefix({ toString: () => "10.0.0.0/8" }).contains("10.1.1.1")), true, "Prefix from an object's toString");
    eq(tryv(() => new netipMod.Prefix("10.0.0.0/8").contains({ toString: () => "10.2.3.4" })), true, "contains converts its argument");
});

await t.run();
