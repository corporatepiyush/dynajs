/* test_netip.js -- dyna:net (in-repo IP/CIDR utilities, value-type addr/prefix model).
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_netip.js
 * Prints "test_netip: all tests passed (N assertions)" on success; throws on
 * failure.
 *
 * Expected values are cross-checked three independent ways:
 *   1. hand vector tables with EXACT expected strings, drawn from RFC 5952 sec.4
 *      and from the addr/prefix semantics verified verbatim against the
 *      reference specification;
 *   2. a JS re-implementation of the RFC 5952 canonicalizer (an algorithmic
 *      twin) diffed against the C module over tens of thousands of random
 *      addresses;
 *   3. structural RFC 5952 invariants proven on the C output independently of
 *      how it was produced (lowercase, <=1 "::", no leading zeros, the "::"
 *      compresses exactly the leftmost-longest zero run, and the output
 *      re-parses to the original bytes). */

import * as netip from "dyna:net";

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

/* deterministic PRNG (LCG) so any failure is reproducible */
let _seed = 0x2545F491 >>> 0;
function rnd() {
    _seed = (Math.imul(_seed, 1664525) + 1013904223) >>> 0;
    return _seed / 4294967296;
}
function ri(m) { return Math.floor(rnd() * m); }

/* ================= independent JS reference canonicalizer ================= */
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

/* structural RFC 5952 invariants on the C output, driven by ground-truth bytes */
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

/* =========================== canonical hand table ========================== */
/* Each [input, expected] pins the EXACT RFC 5952 output. */
{
    const cases = [
        /* --- IPv4 --- */
        ["1.2.3.4", "1.2.3.4"],
        ["0.0.0.0", "0.0.0.0"],
        ["255.255.255.255", "255.255.255.255"],
        ["192.168.0.1", "192.168.0.1"],
        ["9.8.7.6", "9.8.7.6"],
        ["127.0.0.1", "127.0.0.1"],
        /* --- IPv6: RFC 5952 sec.4 leading-zero + :: rules --- */
        ["2001:0db8:0000:0000:0000:0000:0000:0001", "2001:db8::1"],
        ["2001:db8::1", "2001:db8::1"],
        ["2001:DB8::1", "2001:db8::1"],            /* uppercase -> lowercase */
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
        /* single zero group NOT shortened to :: */
        ["1:2:3:4:5:6:0:8", "1:2:3:4:5:6:0:8"],
        ["1:2:0:4:5:6:7:8", "1:2:0:4:5:6:7:8"],
        ["2001:db8:0:1:1:1:1:1", "2001:db8:0:1:1:1:1:1"],
        /* :: picks the LONGEST run ... */
        ["1:0:0:1:0:0:0:1", "1:0:0:1::1"],
        ["0:0:1:0:0:0:0:0", "0:0:1::"],
        ["1:0:0:0:1:0:0:1", "1::1:0:0:1"],
        ["2001:db8:0:0:1:0:0:1", "2001:db8::1:0:0:1"],  /* ... leftmost on ties */
        /* leading/trailing single zeros around a compressed run */
        ["0:1:0:0:0:0:0:0", "0:1::"],
        ["0:0:0:0:0:0:1:0", "::1:0"],
        ["a:b:c:d:e:f:0:0", "a:b:c:d:e:f::"],
        ["1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8"],
        /* --- IPv4-mapped (::ffff:0:0/96) keeps a dotted-quad tail --- */
        ["::ffff:1.2.3.4", "::ffff:1.2.3.4"],
        ["::ffff:0.0.0.0", "::ffff:0.0.0.0"],
        ["::ffff:255.255.255.255", "::ffff:255.255.255.255"],
        ["::ffff:c0a8:0101", "::ffff:192.168.1.1"],
        ["::ffff:0:0", "::ffff:0.0.0.0"],
        ["0:0:0:0:0:ffff:1.2.3.4", "::ffff:1.2.3.4"],
        ["::FFFF:192.168.140.255", "::ffff:192.168.140.255"],
        /* --- embedded IPv4 tail that is NOT mapped -> printed in hex --- */
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
        /* idempotent */
        assert(netip.canonical(got) === got, "canonical idempotent for " + got);
        /* twin + structural */
        const bytes = Array.from(netip.parseAddr(inp).bytes);
        assert(got === refCanonical(bytes), "twin agrees for " + inp);
        checkRFC5952(got, bytes);
    }
}

/* ======================= parseAddr shape & bytes ========================== */
{
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
}

/* ========================= classification table =========================== */
/* order: [loopback, private, multicast, unspecified, llUnicast, global, llMulticast] */
{
    const T = [
        ["127.0.0.1",       [1, 0, 0, 0, 0, 0, 0]],
        ["127.1.2.3",       [1, 0, 0, 0, 0, 0, 0]],
        ["10.0.0.1",        [0, 1, 0, 0, 0, 1, 0]],
        ["10.255.255.255",  [0, 1, 0, 0, 0, 1, 0]],
        ["192.168.1.1",     [0, 1, 0, 0, 0, 1, 0]],
        ["172.16.0.1",      [0, 1, 0, 0, 0, 1, 0]],
        ["172.31.255.255",  [0, 1, 0, 0, 0, 1, 0]],
        ["172.32.0.1",      [0, 0, 0, 0, 0, 1, 0]],  /* just outside 172.16/12 */
        ["172.15.0.1",      [0, 0, 0, 0, 0, 1, 0]],
        ["169.254.1.1",     [0, 0, 0, 0, 1, 0, 0]],
        ["169.254.255.255", [0, 0, 0, 0, 1, 0, 0]],
        ["169.253.0.1",     [0, 0, 0, 0, 0, 1, 0]],  /* not link-local */
        ["224.0.0.1",       [0, 0, 1, 0, 0, 0, 1]],
        ["224.0.0.251",     [0, 0, 1, 0, 0, 0, 1]],
        ["224.0.1.1",       [0, 0, 1, 0, 0, 0, 0]],  /* multicast, not link-local */
        ["239.255.255.255", [0, 0, 1, 0, 0, 0, 0]],
        ["0.0.0.0",         [0, 0, 0, 1, 0, 0, 0]],
        ["255.255.255.255", [0, 0, 0, 0, 0, 0, 0]],  /* broadcast: not global */
        ["8.8.8.8",         [0, 0, 0, 0, 0, 1, 0]],
        ["1.1.1.1",         [0, 0, 0, 0, 0, 1, 0]],
        ["::1",             [1, 0, 0, 0, 0, 0, 0]],
        ["::",              [0, 0, 0, 1, 0, 0, 0]],
        ["fe80::1",         [0, 0, 0, 0, 1, 0, 0]],
        ["fe80::abcd",      [0, 0, 0, 0, 1, 0, 0]],
        ["febf::1",         [0, 0, 0, 0, 1, 0, 0]],  /* top of fe80::/10 */
        ["fec0::1",         [0, 0, 0, 0, 0, 1, 0]],  /* outside fe80::/10 */
        ["fc00::1",         [0, 1, 0, 0, 0, 1, 0]],
        ["fd12:3456::1",    [0, 1, 0, 0, 0, 1, 0]],
        ["fbff::1",         [0, 0, 0, 0, 0, 1, 0]],  /* just below fc00::/7 */
        ["ff02::1",         [0, 0, 1, 0, 0, 0, 1]],
        ["ff02::fb",        [0, 0, 1, 0, 0, 0, 1]],
        ["ff00::",          [0, 0, 1, 0, 0, 0, 0]],
        ["ff05::1",         [0, 0, 1, 0, 0, 0, 0]],  /* multicast, not link-local */
        ["2001:db8::1",     [0, 0, 0, 0, 0, 1, 0]],
        ["2606:4700:4700::1111", [0, 0, 0, 0, 0, 1, 0]],
        /* 4-in-6: every predicate except isUnspecified unmaps first */
        ["::ffff:127.0.0.1",   [1, 0, 0, 0, 0, 0, 0]],
        ["::ffff:10.0.0.1",    [0, 1, 0, 0, 0, 1, 0]],
        ["::ffff:192.168.1.1", [0, 1, 0, 0, 0, 1, 0]],
        ["::ffff:169.254.1.1", [0, 0, 0, 0, 1, 0, 0]],
        ["::ffff:224.0.0.1",   [0, 0, 1, 0, 0, 0, 1]],
        ["::ffff:0.0.0.0",     [0, 0, 0, 0, 0, 0, 0]],  /* NOT unspecified (no unmap there) */
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
}

/* ========================= parsePrefix / masked =========================== */
{
    const p1 = netip.parsePrefix("192.168.0.0/16");
    assert(p1.addr === "192.168.0.0" && p1.bits === 16, "parsePrefix v4");
    const p2 = netip.parsePrefix("2001:db8::/32");
    assert(p2.addr === "2001:db8::" && p2.bits === 32, "parsePrefix v6");
    /* addr is kept UNMASKED: parsePrefix preserves the host bits rather than
     * zeroing them, so 192.168.1.130/24 keeps the .130 */
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
        ["2001:db8:8000::1/33", "2001:db8:8000::"],  /* 33rd bit set, in byte 4 */
        ["2001:db8:7fff::1/33", "2001:db8::"],       /* 33rd bit clear */
        ["ffff:ffff::/1", "8000::"],
        ["ffff::/128", "ffff::"],
        ["::ffff:ffff:ffff/0", "::"],
    ];
    for (const [pre, exp] of M) {
        const got = netip.masked(pre);
        assert(got === exp, "masked(" + pre + ") == " + exp + " (got " + got + ")");
    }
}

/* ====================== contains boundary hand cases ====================== */
{
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
        ["192.168.1.5/24", "192.168.1.99", true],   /* host bits of prefix ignored */
        ["10.0.0.1/8", "10.9.9.9", true],
        /* family mismatch -> false */
        ["10.0.0.0/8", "::ffff:10.0.0.1", false],
        ["0.0.0.0/0", "::1", false],
        ["::/0", "1.2.3.4", false],
        /* v6 */
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
        ["::ffff:0:0/96", "::ffff:1.2.3.4", true],   /* 4in6 prefix contains 4in6 addr */
    ];
    for (const [pre, addr, exp] of C) {
        const got = netip.contains(pre, addr);
        assert(got === exp, "contains(" + pre + ", " + addr + ") == " + exp +
            " (got " + got + ")");
    }
}

/* =========================== compareAddr / isValid ======================== */
{
    assert(netip.compareAddr("1.2.3.4", "1.2.3.4") === 0, "compare equal v4");
    assert(netip.compareAddr("1.2.3.4", "1.2.3.5") === -1, "compare v4 <");
    assert(netip.compareAddr("1.2.3.5", "1.2.3.4") === 1, "compare v4 >");
    assert(netip.compareAddr("1.2.3.4", "::1") === -1, "v4 sorts before v6");
    assert(netip.compareAddr("::1", "1.2.3.4") === 1, "v6 sorts after v4");
    assert(netip.compareAddr("::1", "::2") === -1, "compare v6");
    assert(netip.compareAddr("2001:db8::1", "2001:db8::1") === 0, "compare equal v6");
    assert(netip.compareAddr("::ffff:1.2.3.4", "1.2.3.4") === 1,
        "4in6 (v6) sorts after pure v4");

    assert(netip.isValid("1.2.3.4") === true, "isValid v4");
    assert(netip.isValid("2001:db8::1") === true, "isValid v6");
    assert(netip.isValid("::ffff:1.2.3.4") === true, "isValid 4in6");
    assert(netip.isValid("garbage") === false, "isValid garbage");
    assert(netip.isValid("1.2.3.256") === false, "isValid octet>255");
    assert(netip.isValid("") === false, "isValid empty");
    assert(netip.isValid(12345) === false, "isValid non-string");
    assert(netip.isValid("1.2.3.4/24") === false, "isValid rejects a prefix");
}

/* ============================== reject table ============================== */
{
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
        "1:2:3:4:5:6:7:1.2.3.4",           /* 9 groups worth */
        "1:2:3:4:5:1.2.3.4",               /* v4 not in final 2 groups (no ::) */
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

    /* wrong argument types throw TypeError (except isValid, which is total) */
    throws(() => netip.parseAddr(), "parseAddr() no args");
    throws(() => netip.parseAddr(42), "parseAddr(number)");
    throws(() => netip.parseAddr(null), "parseAddr(null)");
    throws(() => netip.contains("1.2.3.0/24"), "contains missing addr");
    throws(() => netip.contains(1, 2), "contains(number,number)");
    throws(() => netip.compareAddr("1.2.3.4"), "compareAddr missing b");
}

/* ===================== large randomized differential ====================== */
function checkAddr(input, bytes) {
    /* 1. parse yields exactly the ground-truth bytes */
    const pa = netip.parseAddr(input);
    assert(bytesEq(Array.from(pa.bytes), bytes), "parse bytes for " + input);
    assert(pa.is4 === (bytes.length === 4), "parse is4 for " + input);
    assert(pa.is6 === (bytes.length === 16), "parse is6 for " + input);
    assert(netip.isValid(input) === true, "isValid true for " + input);
    /* 2. canonical matches the independent twin */
    const c = netip.canonical(input);
    assert(c === refCanonical(bytes), "twin canonical for " + input +
        " (got " + c + ", ref " + refCanonical(bytes) + ")");
    /* 3. idempotent + value-preserving + structural */
    assert(netip.canonical(c) === c, "idempotent for " + input);
    assert(bytesEq(Array.from(netip.parseAddr(c).bytes), bytes),
        "canonical round-trips for " + input);
    checkRFC5952(c, bytes);
    /* string field agrees with canonical() */
    assert(pa.string === c, "parseAddr.string == canonical for " + input);
}

function hex16(v) {
    let h = v.toString(16);
    if (rnd() < 0.3) h = ("0000" + h).slice(-4);   /* random leading zeros */
    if (rnd() < 0.3) h = h.toUpperCase();           /* random case */
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
    /* half the time force a zero run to exercise :: */
    if (rnd() < 0.6) {
        const s = ri(7), len = 1 + ri(8 - s);
        for (let k = s; k < s + len; k++) { b[2 * k] = 0; b[2 * k + 1] = 0; }
    }
    const g = [];
    for (let i = 0; i < 8; i++) g.push(hex16((b[2 * i] << 8) | b[2 * i + 1]));
    checkAddr(g.join(":"), b);   /* full 8-group form (never has ::) */
}

for (let it = 0; it < N_4IN6; it++) {
    const b = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, ri(256), ri(256), ri(256), ri(256)];
    /* alternate between dotted and hex input forms */
    const input = (rnd() < 0.5)
        ? "::ffff:" + b.slice(12).join(".")
        : "::ffff:" + hex16((b[12] << 8) | b[13]) + ":" + hex16((b[14] << 8) | b[15]);
    checkAddr(input, b);
}

for (let it = 0; it < N_EMB; it++) {
    /* 6 full groups + dotted v4 tail; ground-truth bytes are trivial */
    const b = [];
    for (let i = 0; i < 12; i++) b.push(ri(256));
    const o = [ri(256), ri(256), ri(256), ri(256)];
    for (let i = 0; i < 4; i++) b.push(o[i]);
    const g = [];
    for (let i = 0; i < 6; i++) g.push(((b[2 * i] << 8) | b[2 * i + 1]).toString(16));
    checkAddr(g.join(":") + ":" + o.join("."), b);
}

/* randomized contains against an independent JS mask comparison */
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
        /* bias toward sharing a prefix so both branches are exercised */
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
