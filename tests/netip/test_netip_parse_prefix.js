// covers: dyna:netip.parsePrefix
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_netip_parse_prefix");
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

__kitS.section("parsePrefix v4", () => {
    const p1 = netip.parsePrefix("192.168.0.0/16");
    assert(p1.addr === "192.168.0.0" && p1.bits === 16, "parsePrefix v4");
    const p2 = netip.parsePrefix("2001:db8::/32");
    assert(p2.addr === "2001:db8::" && p2.bits === 32, "parsePrefix v6");
    const p3 = netip.parsePrefix("192.168.1.130/24");
    assert(p3.addr === "192.168.1.130" && p3.bits === 24, "parsePrefix keeps host bits");
    assert(netip.parsePrefix("1.2.3.4/0").bits === 0, "parsePrefix /0");
    assert(netip.parsePrefix("1.2.3.4/32").bits === 32, "parsePrefix /32");
    assert(netip.parsePrefix("::/0").bits === 0, "parsePrefix ::/0");
    assert(netip.parsePrefix("::/128").bits === 128, "parsePrefix ::/128");

    const M = [
        ["192.168.1.130/24", "192.168.1.0"],
        ["192.168.1.130/25", "192.168.1.128"],
        ["10.11.12.13/8", "10.0.0.0"],
        ["10.11.12.13/12", "10.0.0.0"],
        ["172.16.5.4/12", "172.16.0.0"],
        ["1.2.200.5/17", "1.2.128.0"],
        ["255.255.255.255/1", "128.0.0.0"],
        ["255.255.255.255/0", "0.0.0.0"],
        ["255.255.255.255/32", "255.255.255.255"],
        ["1.2.3.4/31", "1.2.3.4"],
        ["1.2.3.5/31", "1.2.3.4"],
        ["2001:db8:1:2::/32", "2001:db8::"],
        ["2001:db8:abcd::1/48", "2001:db8:abcd::"],
        ["2001:db8:abcd:ef01::1/56", "2001:db8:abcd:ef00::"],
        ["2001:db8::ffff/33", "2001:db8::"],
        ["2001:db8:8000::1/33", "2001:db8:8000::"],
        ["2001:db8:7fff::1/33", "2001:db8::"],
        ["ffff:ffff::/1", "8000::"],
        ["ffff::/128", "ffff::"],
        ["::ffff:ffff:ffff/0", "::"],
    ];
    for (const [pre, exp] of M) {
        const got = netip.masked(pre);
        assert(got === exp, "masked(" + pre + ") == " + exp + " (got " + got + ")");
    }
});
__kitS.end();

print("test_netip: all tests passed (" + n + " assertions)");
