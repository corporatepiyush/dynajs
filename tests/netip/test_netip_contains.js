// covers: dyna:netip.contains
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_netip_contains");
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

__kitS.section("(got", () => {
    const C = [
        ["10.0.0.0/12", "10.15.255.255", true],
        ["10.0.0.0/12", "10.16.0.0", false],
        ["10.0.0.0/12", "10.0.0.0", true],
        ["1.2.128.0/17", "1.2.128.0", true],
        ["1.2.128.0/17", "1.2.255.255", true],
        ["1.2.128.0/17", "1.2.127.255", false],
        ["1.2.128.0/17", "1.3.0.0", false],
        ["0.0.0.0/0", "255.255.255.255", true],
        ["0.0.0.0/0", "0.0.0.0", true],
        ["192.168.1.0/24", "192.168.1.255", true],
        ["192.168.1.0/24", "192.168.2.0", false],
        ["192.168.1.0/31", "192.168.1.1", true],
        ["192.168.1.0/31", "192.168.1.2", false],
        ["192.168.1.5/24", "192.168.1.99", true],
        ["10.0.0.1/8", "10.9.9.9", true],
        ["10.0.0.0/8", "::ffff:10.0.0.1", false],
        ["0.0.0.0/0", "::1", false],
        ["::/0", "1.2.3.4", false],
        ["2001:db8::/32", "2001:db8:ffff::1", true],
        ["2001:db8::/32", "2001:db9::1", false],
        ["2001:db8::/33", "2001:db8:7fff::", true],
        ["2001:db8::/33", "2001:db8:8000::", false],
        ["::/0", "2001:db8::1", true],
        ["fe80::/10", "fe80::1", true],
        ["fe80::/10", "febf:ffff::1", true],
        ["fe80::/10", "fec0::1", false],
        ["fc00::/7", "fdff::1", true],
        ["fc00::/7", "fbff::1", false],
        ["2001:db8::/128", "2001:db8::", true],
        ["2001:db8::/128", "2001:db8::1", false],
        ["::ffff:0:0/96", "::ffff:1.2.3.4", true],
    ];
    for (const [pre, addr, exp] of C) {
        const got = netip.contains(pre, addr);
        assert(got === exp, "contains(" + pre + ", " + addr + ") == " + exp +
            " (got " + got + ")");
    }
});
__kitS.end();

const N_V4 = 2500, N_V6 = 3500, N_4IN6 = 1200, N_EMB = 1200, N_CONTAINS = 3000;

function refContains(prefixStr, addrStr) {
    const pp = netip.parsePrefix(prefixStr);
    const pa = netip.parseAddr(pp.addr);
    const ia = netip.parseAddr(addrStr);
    if (pa.is4 !== ia.is4) return false;
    const pb = Array.from(pa.bytes), ib = Array.from(ia.bytes);
    let bits = pp.bits;
    for (let i = 0; i < pb.length; i++) {
        const take = Math.min(8, bits);
        if (take <= 0) break;
        const mask = (0xff << (8 - take)) & 0xff;
        if ((pb[i] & mask) !== (ib[i] & mask)) return false;
        bits -= 8;
    }
    return true;
}

for (let it = 0; it < N_CONTAINS; it++) {
    const v4 = rnd() < 0.5;
    let pre, addr;
    if (v4) {
        const nb = [ri(256), ri(256), ri(256), ri(256)];
        const ab = [ri(256), ri(256), ri(256), ri(256)];
        if (rnd() < 0.5) { ab[0] = nb[0]; if (rnd() < 0.6) ab[1] = nb[1]; }
        pre = nb.join(".") + "/" + ri(33);
        addr = ab.join(".");
    } else {
        const nb = [], ab = [];
        for (let i = 0; i < 16; i++) { nb.push(ri(256)); ab.push(ri(256)); }
        if (rnd() < 0.5) for (let i = 0; i < 4 + ri(8); i++) ab[i] = nb[i];
        const gs = a => { const g = []; for (let i = 0; i < 8; i++) g.push(((a[2 * i] << 8) | a[2 * i + 1]).toString(16)); return g.join(":"); };
        pre = gs(nb) + "/" + ri(129);
        addr = gs(ab);
    }
    assert(netip.contains(pre, addr) === refContains(pre, addr),
        "contains twin for " + pre + " / " + addr);
}

print("test_netip: all tests passed (" + n + " assertions)");
