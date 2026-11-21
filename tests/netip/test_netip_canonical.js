// covers: dyna:netip.canonical
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_netip_canonical");
import * as netip from "dyna:netip";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throws(fn, msg) {
    let caught = false;
    try { fn(); } catch (e) { caught = true; }
    assert(caught, "should throw: " + msg);
}
function bytesEq(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}

let _seed = 0x2545F491 >>> 0;
function rnd() {
    _seed = (Math.imul(_seed, 1664525) + 1013904223) >>> 0;
    return _seed / 4294967296;
}
function ri(m) { return Math.floor(rnd() * m); }

function refCanonical(bytes) {
    if (bytes.length === 4) return bytes.join(".");
    let m = true;
    for (let i = 0; i < 10; i++) if (bytes[i] !== 0) m = false;
    if (bytes[10] !== 0xff || bytes[11] !== 0xff) m = false;
    if (m) return "::ffff:" + bytes.slice(12).join(".");
    const g = [];
    for (let i = 0; i < 8; i++) g.push((bytes[2 * i] << 8) | bytes[2 * i + 1]);
    let bs = -1, bl = 0;
    for (let i = 0; i < 8;) {
        if (g[i] === 0) {
            let j = i;
            while (j < 8 && g[j] === 0) j++;
            if (j - i > bl) { bl = j - i; bs = i; }
            i = j;
        } else i++;
    }
    if (bl < 2) bs = -1;
    let out = "";
    for (let i = 0; i < 8; i++) {
        if (i === bs) { out += "::"; i = bs + bl; if (i >= 8) break; }
        else if (i > 0) out += ":";
        out += g[i].toString(16);
    }
    return out;
}

function checkRFC5952(str, bytes) {
    assert(str === str.toLowerCase(), "canonical is lowercase: " + str);
    if (bytes.length === 4) {
        assert(str === bytes.join("."), "v4 canonical dotted: " + str);
        return;
    }
    let m = true;
    for (let i = 0; i < 10; i++) if (bytes[i] !== 0) m = false;
    if (bytes[10] !== 0xff || bytes[11] !== 0xff) m = false;
    if (m) {
        assert(str === "::ffff:" + bytes.slice(12).join("."),
            "4in6 canonical has dotted tail: " + str);
        return;
    }
    const dc = str.split("::").length - 1;
    assert(dc <= 1, "at most one :: in " + str);
    const g = [];
    for (let i = 0; i < 8; i++) g.push((bytes[2 * i] << 8) | bytes[2 * i + 1]);
    const runs = [];
    for (let i = 0; i < 8;) {
        if (g[i] === 0) {
            let j = i;
            while (j < 8 && g[j] === 0) j++;
            runs.push({ s: i, l: j - i });
            i = j;
        } else i++;
    }
    let maxLen = 0;
    for (const r of runs) if (r.l >= 2 && r.l > maxLen) maxLen = r.l;
    let expStart = -1;
    if (maxLen > 0) for (const r of runs) if (r.l === maxLen) { expStart = r.s; break; }
    let cs = -1, cl = 0;
    if (dc === 1) {
        const parts = str.split("::");
        const lc = parts[0] === "" ? 0 : parts[0].split(":").length;
        const rc = parts[1] === "" ? 0 : parts[1].split(":").length;
        cs = lc; cl = 8 - lc - rc;
    }
    if (maxLen === 0) {
        assert(dc === 0, "no :: expected (no zero run >=2) for " + str);
    } else {
        assert(cs === expStart && cl === maxLen,
            "leftmost-longest :: for " + str + " (got " + cs + "/" + cl +
            ", exp " + expStart + "/" + maxLen + ")");
    }
    const toks = str.replace("::", ":").split(":").filter(x => x.length > 0);
    for (const t of toks) {
        assert(/^[0-9a-f]{1,4}$/.test(t), "valid hex group '" + t + "' in " + str);
        assert(t.length === 1 || t[0] !== "0", "no leading zero in '" + t + "' (" + str + ")");
    }
    const rb = Array.from(netip.parseAddr(str).bytes);
    assert(bytesEq(rb, bytes), "canonical re-parses to same bytes: " + str);
}

__kitS.section("invalid:", () => {
    const cases = [
        ["1.2.3.4", "1.2.3.4"],
        ["0.0.0.0", "0.0.0.0"],
        ["255.255.255.255", "255.255.255.255"],
        ["192.168.0.1", "192.168.0.1"],
        ["9.8.7.6", "9.8.7.6"],
        ["127.0.0.1", "127.0.0.1"],
        ["2001:0db8:0000:0000:0000:0000:0000:0001", "2001:db8::1"],
        ["2001:db8::1", "2001:db8::1"],
        ["2001:DB8::1", "2001:db8::1"],
        ["2001:0DB8::0001", "2001:db8::1"],
        ["::", "::"],
        ["0:0:0:0:0:0:0:0", "::"],
        ["::1", "::1"],
        ["0:0:0:0:0:0:0:1", "::1"],
        ["1::", "1::"],
        ["1:0:0:0:0:0:0:0", "1::"],
        ["fe80::1", "fe80::1"],
        ["fe80:0:0:0:0:0:0:1", "fe80::1"],
        ["fe80:0000:0000:0000:0000:0000:0000:0001", "fe80::1"],
        ["1:2:3:4:5:6:0:8", "1:2:3:4:5:6:0:8"],
        ["1:2:0:4:5:6:7:8", "1:2:0:4:5:6:7:8"],
        ["2001:db8:0:1:1:1:1:1", "2001:db8:0:1:1:1:1:1"],
        ["1:0:0:1:0:0:0:1", "1:0:0:1::1"],
        ["0:0:1:0:0:0:0:0", "0:0:1::"],
        ["1:0:0:0:1:0:0:1", "1::1:0:0:1"],
        ["2001:db8:0:0:1:0:0:1", "2001:db8::1:0:0:1"],
        ["0:1:0:0:0:0:0:0", "0:1::"],
        ["0:0:0:0:0:0:1:0", "::1:0"],
        ["a:b:c:d:e:f:0:0", "a:b:c:d:e:f::"],
        ["1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8"],
        ["::ffff:1.2.3.4", "::ffff:1.2.3.4"],
        ["::ffff:0.0.0.0", "::ffff:0.0.0.0"],
        ["::ffff:255.255.255.255", "::ffff:255.255.255.255"],
        ["::ffff:c0a8:0101", "::ffff:192.168.1.1"],
        ["::ffff:0:0", "::ffff:0.0.0.0"],
        ["0:0:0:0:0:ffff:1.2.3.4", "::ffff:1.2.3.4"],
        ["::FFFF:192.168.140.255", "::ffff:192.168.140.255"],
        ["::1.2.3.4", "::102:304"],
        ["2001:db8::1.2.3.4", "2001:db8::102:304"],
        ["64:ff9b::1.2.3.4", "64:ff9b::102:304"],
        ["::0.0.0.0", "::"],
    ];
    for (const [inp, exp] of cases) {
        if (exp === null) { throws(() => netip.canonical(inp), "invalid: " + inp); continue; }
        const got = netip.canonical(inp);
        assert(got === exp, "canonical(" + JSON.stringify(inp) + ") == " +
            JSON.stringify(exp) + " (got " + JSON.stringify(got) + ")");
        assert(netip.canonical(got) === got, "canonical idempotent for " + got);
        const bytes = Array.from(netip.parseAddr(inp).bytes);
        assert(got === refCanonical(bytes), "twin agrees for " + inp);
        checkRFC5952(got, bytes);
    }
});
__kitS.end();

function checkAddr(input, bytes) {
    const pa = netip.parseAddr(input);
    assert(bytesEq(Array.from(pa.bytes), bytes), "parse bytes for " + input);
    assert(pa.is4 === (bytes.length === 4), "parse is4 for " + input);
    assert(pa.is6 === (bytes.length === 16), "parse is6 for " + input);
    assert(netip.isValid(input) === true, "isValid true for " + input);
    const c = netip.canonical(input);
    assert(c === refCanonical(bytes), "twin canonical for " + input +
        " (got " + c + ", ref " + refCanonical(bytes) + ")");
    assert(netip.canonical(c) === c, "idempotent for " + input);
    assert(bytesEq(Array.from(netip.parseAddr(c).bytes), bytes),
        "canonical round-trips for " + input);
    checkRFC5952(c, bytes);
    assert(pa.string === c, "parseAddr.string == canonical for " + input);
}

function hex16(v) {
    let h = v.toString(16);
    if (rnd() < 0.3) h = ("0000" + h).slice(-4);
    if (rnd() < 0.3) h = h.toUpperCase();
    return h;
}

const N_V4 = 2500, N_V6 = 3500, N_4IN6 = 1200, N_EMB = 1200, N_CONTAINS = 3000;

for (let it = 0; it < N_V4; it++) {
    const b = [ri(256), ri(256), ri(256), ri(256)];
    checkAddr(b.join("."), b);
}

for (let it = 0; it < N_V6; it++) {
    const b = [];
    for (let i = 0; i < 16; i++) b.push(ri(256));
    if (rnd() < 0.6) {
        const s = ri(7), len = 1 + ri(8 - s);
        for (let k = s; k < s + len; k++) { b[2 * k] = 0; b[2 * k + 1] = 0; }
    }
    const g = [];
    for (let i = 0; i < 8; i++) g.push(hex16((b[2 * i] << 8) | b[2 * i + 1]));
    checkAddr(g.join(":"), b);
}

for (let it = 0; it < N_4IN6; it++) {
    const b = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, ri(256), ri(256), ri(256), ri(256)];
    const input = (rnd() < 0.5)
        ? "::ffff:" + b.slice(12).join(".")
        : "::ffff:" + hex16((b[12] << 8) | b[13]) + ":" + hex16((b[14] << 8) | b[15]);
    checkAddr(input, b);
}

for (let it = 0; it < N_EMB; it++) {
    const b = [];
    for (let i = 0; i < 12; i++) b.push(ri(256));
    const o = [ri(256), ri(256), ri(256), ri(256)];
    for (let i = 0; i < 4; i++) b.push(o[i]);
    const g = [];
    for (let i = 0; i < 6; i++) g.push(((b[2 * i] << 8) | b[2 * i + 1]).toString(16));
    checkAddr(g.join(":") + ":" + o.join("."), b);
}

print("test_netip: all tests passed (" + n + " assertions)");
