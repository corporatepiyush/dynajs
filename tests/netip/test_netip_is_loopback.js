// covers: dyna:netip.isLoopback
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_netip_is_loopback");
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
    const T = [
        ["127.0.0.1",       [1, 0, 0, 0, 0, 0, 0]],
        ["127.1.2.3",       [1, 0, 0, 0, 0, 0, 0]],
        ["10.0.0.1",        [0, 1, 0, 0, 0, 1, 0]],
        ["10.255.255.255",  [0, 1, 0, 0, 0, 1, 0]],
        ["192.168.1.1",     [0, 1, 0, 0, 0, 1, 0]],
        ["172.16.0.1",      [0, 1, 0, 0, 0, 1, 0]],
        ["172.31.255.255",  [0, 1, 0, 0, 0, 1, 0]],
        ["172.32.0.1",      [0, 0, 0, 0, 0, 1, 0]],
        ["172.15.0.1",      [0, 0, 0, 0, 0, 1, 0]],
        ["169.254.1.1",     [0, 0, 0, 0, 1, 0, 0]],
        ["169.254.255.255", [0, 0, 0, 0, 1, 0, 0]],
        ["169.253.0.1",     [0, 0, 0, 0, 0, 1, 0]],
        ["224.0.0.1",       [0, 0, 1, 0, 0, 0, 1]],
        ["224.0.0.251",     [0, 0, 1, 0, 0, 0, 1]],
        ["224.0.1.1",       [0, 0, 1, 0, 0, 0, 0]],
        ["239.255.255.255", [0, 0, 1, 0, 0, 0, 0]],
        ["0.0.0.0",         [0, 0, 0, 1, 0, 0, 0]],
        ["255.255.255.255", [0, 0, 0, 0, 0, 0, 0]],
        ["8.8.8.8",         [0, 0, 0, 0, 0, 1, 0]],
        ["1.1.1.1",         [0, 0, 0, 0, 0, 1, 0]],
        ["::1",             [1, 0, 0, 0, 0, 0, 0]],
        ["::",              [0, 0, 0, 1, 0, 0, 0]],
        ["fe80::1",         [0, 0, 0, 0, 1, 0, 0]],
        ["fe80::abcd",      [0, 0, 0, 0, 1, 0, 0]],
        ["febf::1",         [0, 0, 0, 0, 1, 0, 0]],
        ["fec0::1",         [0, 0, 0, 0, 0, 1, 0]],
        ["fc00::1",         [0, 1, 0, 0, 0, 1, 0]],
        ["fd12:3456::1",    [0, 1, 0, 0, 0, 1, 0]],
        ["fbff::1",         [0, 0, 0, 0, 0, 1, 0]],
        ["ff02::1",         [0, 0, 1, 0, 0, 0, 1]],
        ["ff02::fb",        [0, 0, 1, 0, 0, 0, 1]],
        ["ff00::",          [0, 0, 1, 0, 0, 0, 0]],
        ["ff05::1",         [0, 0, 1, 0, 0, 0, 0]],
        ["2001:db8::1",     [0, 0, 0, 0, 0, 1, 0]],
        ["2606:4700:4700::1111", [0, 0, 0, 0, 0, 1, 0]],
        ["::ffff:127.0.0.1",   [1, 0, 0, 0, 0, 0, 0]],
        ["::ffff:10.0.0.1",    [0, 1, 0, 0, 0, 1, 0]],
        ["::ffff:192.168.1.1", [0, 1, 0, 0, 0, 1, 0]],
        ["::ffff:169.254.1.1", [0, 0, 0, 0, 1, 0, 0]],
        ["::ffff:224.0.0.1",   [0, 0, 1, 0, 0, 0, 1]],
        ["::ffff:0.0.0.0",     [0, 0, 0, 0, 0, 0, 0]],
        ["::ffff:8.8.8.8",     [0, 0, 0, 0, 0, 1, 0]],
    ];
    const fns = [netip.isLoopback, netip.isPrivate, netip.isMulticast,
        netip.isUnspecified, netip.isLinkLocalUnicast, netip.isGlobalUnicast,
        netip.isLinkLocalMulticast];
    const names = ["isLoopback", "isPrivate", "isMulticast", "isUnspecified",
        "isLinkLocalUnicast", "isGlobalUnicast", "isLinkLocalMulticast"];
    for (const [addr, exp] of T) {
        for (let k = 0; k < fns.length; k++) {
            const got = fns[k](addr);
            assert(got === !!exp[k],
                names[k] + "(" + addr + ") == " + !!exp[k] + " (got " + got + ")");
        }
    }
});
__kitS.end();

print("test_netip: all tests passed (" + n + " assertions)");
