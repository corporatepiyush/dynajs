// covers: dyna:netip.parseAddr
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_netip_parse_addr");
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

__kitS.section("1.2.3.4 is4", () => {
    const a = netip.parseAddr("1.2.3.4");
    assert(a.is4 === true && a.is6 === false, "1.2.3.4 is4");
    assert(a.bytes.length === 4 && bytesEq(Array.from(a.bytes), [1, 2, 3, 4]), "v4 bytes");
    assert(a.string === "1.2.3.4", "v4 string");
    assert(a.bytes instanceof Uint8Array, "bytes is Uint8Array");

    const b = netip.parseAddr("2001:db8::1");
    assert(b.is4 === false && b.is6 === true, "v6 is6");
    assert(b.bytes.length === 16, "v6 bytes length 16");
    const expB = [0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1];
    assert(bytesEq(Array.from(b.bytes), expB), "v6 bytes value");

    const c = netip.parseAddr("::ffff:1.2.3.4");
    assert(c.is4 === false && c.is6 === true, "4in6 is6 (not is4)");
    assert(c.bytes.length === 16, "4in6 16 bytes");
    const expC = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 1, 2, 3, 4];
    assert(bytesEq(Array.from(c.bytes), expC), "4in6 bytes value");
    assert(c.string === "::ffff:1.2.3.4", "4in6 string");
});

__kitS.section("2001:db8: :1", () => {
    const bad = [
        "", " ", "  ", "1", "1.2", "1.2.3", "1.2.3.4.5", "1.2.3.",
        ".1.2.3", "1..3.4", "256.1.1.1", "1.256.1.1", "1.2.3.256",
        "01.2.3.4", "1.02.3.4", "1.2.3.04", "1.2.3.4 ", " 1.2.3.4",
        "1.2.3.-1", "1.2.3.x", "0x1.2.3.4", "1.2.3.4:80",
        "999.999.999.999", "1.2.3.400",
        ":", ":::", "1:2:3:4:5:6:7", "1:2:3:4:5:6:7:8:9",
        "1::2::3", "::1::", "12345::", "1:2:3:4:5:6:7:88888",
        "gg::", "1:2:3:4:5:6:7:8:", ":1:2:3:4:5:6:7:8",
        "1:2:3:4:5:6:7:8:", "::12345", "fffff::",
        "::ffff:999.1.1.1", "::ffff:1.2.3", "::ffff:1.2.3.4.5",
        "1.2.3.4::", "::1.2.3", "::1.2.3.4.5",
        "1:2:3:4:5:6:7:1.2.3.4",
        "1:2:3:4:5:1.2.3.4",
        "1.2.3.4%eth0", "fe80::1%eth0", "%eth0", "::%",
        "2001:db8:::1", "2001:db8: :1", "2001:db8::g",
        "-1.2.3.4", "+1.2.3.4",
    ];
    for (const s of bad) {
        throws(() => netip.parseAddr(s), "parseAddr rejects " + JSON.stringify(s));
        assert(netip.isValid(s) === false, "isValid false for " + JSON.stringify(s));
        throws(() => netip.canonical(s), "canonical rejects " + JSON.stringify(s));
    }

    const badPrefix = [
        "1.2.3.4", "1.2.3.4/", "1.2.3.4//", "1.2.3.4/33", "1.2.3.4/321",
        "1.2.3.4/-1", "1.2.3.4/+1", "1.2.3.4/016", "1.2.3.4/00",
        "1.2.3.4/ 1", "1.2.3.4/1 ", "1.2.3.4/abc", "1.2.3.4/0x1",
        "2001:db8::/129", "2001:db8::/", "2001:db8::/999",
        "256.0.0.0/8", "1.2.3.4/32/32", "/16", "1.2.3.4/1e1",
    ];
    for (const s of badPrefix) {
        throws(() => netip.parsePrefix(s), "parsePrefix rejects " + JSON.stringify(s));
        throws(() => netip.masked(s), "masked rejects " + JSON.stringify(s));
    }

    throws(() => netip.parseAddr(), "parseAddr() no args");
    throws(() => netip.parseAddr(42), "parseAddr(number)");
    throws(() => netip.parseAddr(null), "parseAddr(null)");
    throws(() => netip.contains("1.2.3.0/24"), "contains missing addr");
    throws(() => netip.contains(1, 2), "contains(number,number)");
    throws(() => netip.compareAddr("1.2.3.4"), "compareAddr missing b");
});
__kitS.end();

print("test_netip: all tests passed (" + n + " assertions)");
