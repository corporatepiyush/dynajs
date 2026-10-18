import { Range, satisfies, maxSatisfying, minSatisfying } from "dyna:semver";
import { Prefix, contains, masked } from "dyna:net";
import { Matcher } from "dyna:matcher";
import { Hasher, SHA256Hex } from "dyna:hash";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + msg +
                        "\n  got:  " + JSON.stringify(got) +
                        "\n  want: " + JSON.stringify(want));
}
function eqArr(got, want, msg) {
    n++;
    const a = JSON.stringify(got), b = JSON.stringify(want);
    if (a !== b) throw new Error("assertion failed: " + msg + "\n  got:  " + a + "\n  want: " + b);
}
function throwsType(fn, msg) {
    n++;
    try { fn(); } catch (e) {
        if (e instanceof TypeError) return;
        throw new Error("assertion failed (wrong error type): " + msg + " -> " + e);
    }
    throw new Error("assertion failed (expected TypeError): " + msg);
}

{
    for (const [name, C] of [["Range", Range], ["Prefix", Prefix],
                             ["Matcher", Matcher], ["Hasher", Hasher]]) {
        throwsType(() => C("x"), name + " without new must throw TypeError");
        assert(typeof C === "function", name + " is a constructor");
    }
}

const RANGES = [
    "1.2.3", "=1.2.3", ">1.2.3", ">=1.2.3", "<2.0.0", "<=2.0.0",
    "^1.2.3", "^0.2.3", "^0.0.3", "~1.2.3", "~1.2", "~1",
    ">=1.2.3 <2.0.0", "1.2.3 || >=2.0.0", "^1.0.0 || ^2.0.0 || ^3.0.0",
    ">=1.0.0-alpha", "*", ">=0.0.0",
];
const VERSIONS = [
    "0.0.1", "0.1.0", "0.2.3", "0.2.4", "0.3.0", "0.0.3", "0.0.4",
    "1.0.0", "1.2.2", "1.2.3", "1.2.4", "1.3.0", "1.9.9",
    "2.0.0", "2.5.0", "3.0.0", "3.1.4",
    "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-beta", "1.2.3-rc.1",
];

{
    let pairs = 0;
    for (const rs of RANGES) {
        const r = new Range(rs);
        eq(r.source, rs, "Range.source round-trips: " + rs);
        assert(r.setCount >= 1, "a valid range compiles to >=1 comparator set: " + rs);
        for (const v of VERSIONS) {
            eq(r.test(v), satisfies(v, rs), `Range("${rs}").test("${v}") == satisfies()`);
            pairs++;
        }
    }
    assert(pairs === RANGES.length * VERSIONS.length, "cross product covered (" + pairs + ")");
}
{
    const r = new Range(">=1.2.3 <2.0.0");
    const other = new Range("^3.0.0");
    const forward = VERSIONS.map(v => r.test(v));
    const backward = VERSIONS.slice().reverse().map(v => r.test(v)).reverse();
    eqArr(backward, forward, "Range answers do not depend on call order");
    const interleaved = VERSIONS.map(v => { other.test(v); return r.test(v); });
    eqArr(interleaved, forward, "one Range is unaffected by another's use");
    const probe = "1.3.0";
    const probeIdx = VERSIONS.indexOf(probe);
    assert(probeIdx >= 0, "the drift probe is actually in the corpus");
    for (let i = 0; i < 500; i++)
        if (r.test(probe) !== forward[probeIdx])
            throw new Error("Range.test drifted after " + i + " calls");
    n++;
}
{
    const r = new Range("^1.0.0");
    const list = ["1.0.0", "1.5.0", "1.9.9", "2.0.0", "0.9.0"];
    eq(r.maxSatisfying(["1.0.0", "junk"]), "1.0.0",
       "Range.maxSatisfying skips a junk array entry");
    eq(maxSatisfying(["1.0.0", "junk"], "^1.0.0"), "1.0.0",
       "free maxSatisfying skips it the same way");
    eq(JSON.stringify(r.filter(["1.0.0", "junk"])), '["1.0.0"]',
       "Range.filter skips a junk array entry");
    eq(r.maxSatisfying(list), maxSatisfying(list, "^1.0.0"), "maxSatisfying agrees");
    eq(r.minSatisfying(list), minSatisfying(list, "^1.0.0"), "minSatisfying agrees");
    eqArr(r.filter(list), ["1.0.0", "1.5.0", "1.9.9"], "filter keeps input order");
    eq(new Range("^9.0.0").maxSatisfying(list), null, "no match -> null");
    eqArr(new Range("^9.0.0").filter(list), [], "no match -> empty array");
    eqArr(r.filter([]), [], "empty input -> empty array");
    eq(r.maxSatisfying([]), null, "empty input -> null");
}
{
    for (const bad of [">=", "not-a-range", ">>1.0.0", "1.2.3 <", "^", "~>"])
        throwsType(() => new Range(bad), "invalid range rejected at construction: " + JSON.stringify(bad));
    {
        const empty = new Range("");
        for (const v of VERSIONS)
            eq(empty.test(v), satisfies(v, ""), 'empty range == satisfies(v, "") for ' + v);
        eq(empty.source, "", "empty range keeps its source");
    }
    const r = new Range("*");
    eq(r.test("nonsense"), false, "unparseable version -> false, not a throw");
    eq(r.test(""), false, "empty version -> false");
}
{
    const r = new Range(">=1.0.0");
    let innerRan = false, innerResult = null;
    const evil = { toString() { innerRan = true; innerResult = r.test("2.0.0"); return "1.5.0"; } };
    const outer = r.test(evil);
    assert(innerRan, "toString is the coercion hook for Range.test");
    eq(innerResult, true, "the reentrant inner test answers correctly");
    eq(outer, true, "the outer test still answers for its own argument");
    eq(r.test("1.5.0"), true, "Range is intact after reentrant use");
    eq(r.test("0.1.0"), false, "Range is intact after reentrant use (negative)");
}
{
    const r = new Range("^1.0.0");
    let seen = null;
    const hostile = ["1.0.0", null, "2.0.0"];
    Object.defineProperty(hostile, 1, {
        configurable: true, enumerable: true,
        get() { seen = r.test("1.4.0"); return "1.5.0"; },
    });
    const best = r.maxSatisfying(hostile);
    eq(seen, true, "reentrant test during an array getter answers correctly");
    eq(best, "1.5.0", "maxSatisfying still returns the right element");
}

const PREFIXES = [
    "10.0.0.0/8", "192.168.0.0/16", "172.16.0.0/12", "127.0.0.0/8",
    "0.0.0.0/0", "1.2.3.4/32", "10.1.2.0/24",
    "2001:db8::/32", "::/0", "fe80::/10", "::1/128", "2001:db8:abcd::/48",
];
const ADDRS = [
    "10.0.0.1", "10.255.255.255", "11.0.0.1", "9.255.255.255",
    "192.168.1.1", "172.16.5.5", "172.32.0.1", "127.0.0.1", "1.2.3.4", "1.2.3.5",
    "10.1.2.3", "10.1.3.3",
    "2001:db8::1", "2001:db9::1", "fe80::1", "::1", "::", "2001:db8:abcd::5",
];

{
    let pairs = 0;
    for (const ps of PREFIXES) {
        const p = new Prefix(ps);
        eq(p.masked, masked(ps), `Prefix("${ps}").masked == masked()`);
        for (const a of ADDRS) {
            eq(p.contains(a), contains(ps, a), `Prefix("${ps}").contains("${a}")`);
            pairs++;
        }
    }
    assert(pairs === PREFIXES.length * ADDRS.length, "cross product covered (" + pairs + ")");
}
{
    const loose = new Prefix("10.1.2.3/8");
    const tight = new Prefix("10.0.0.0/8");
    eq(loose.masked, tight.masked, "host bits are masked off at construction");
    eq(loose.bits, tight.bits, "prefix length preserved");
    for (const a of ADDRS)
        eq(loose.contains(a), tight.contains(a), "unmasked and masked CIDRs behave identically: " + a);
}
{
    const v4 = new Prefix("10.0.0.0/8"), v6 = new Prefix("2001:db8::/32");
    eq(v4.isIPv4, true, "v4 flag"); eq(v6.isIPv4, false, "v6 flag");
    eq(v4.contains("2001:db8::1"), false, "a v4 prefix never contains a v6 address");
    eq(v6.contains("10.0.0.1"), false, "a v6 prefix never contains a v4 address");
    eq(v4.overlaps(v6), false, "different families never overlap");
    const all4 = new Prefix("0.0.0.0/0"), all6 = new Prefix("::/0");
    eq(all4.contains("255.255.255.255"), true, "/0 contains any v4");
    eq(all6.contains("2001:db8::1"), true, "::/0 contains any v6");
    const host = new Prefix("1.2.3.4/32");
    eq(host.contains("1.2.3.4"), true, "/32 contains exactly its address");
    eq(host.contains("1.2.3.5"), false, "/32 excludes its neighbour");
}
{
    const a = new Prefix("10.0.0.0/8"), b = new Prefix("10.1.0.0/16"),
          c = new Prefix("11.0.0.0/8");
    eq(a.overlaps(a), true, "overlaps is reflexive");
    eq(a.overlaps(b), true, "a /8 overlaps a contained /16");
    eq(b.overlaps(a), true, "overlaps is symmetric");
    eq(a.overlaps(c), false, "disjoint /8s do not overlap");
    eq(c.overlaps(a), false, "disjoint, both directions");
    throwsType(() => a.overlaps("10.1.0.0/16"), "overlaps requires a Prefix, not a string");
}
{
    for (const bad of ["", "10.0.0.0", "10.0.0.0/33", "2001:db8::/129",
                       "10.0.0.0/-1", "10.0.0.0/x", "999.0.0.1/8", "10.0.0.0/08"])
        throwsType(() => new Prefix(bad), "invalid CIDR rejected: " + JSON.stringify(bad));
    const p = new Prefix("10.0.0.0/8");
    for (const bad of ["", "nonsense", "10.0.0", "999.1.1.1", "10.0.0.1%eth0"])
        eq(p.contains(bad), false, "unparseable address -> false: " + JSON.stringify(bad));
}
{
    const p = new Prefix("10.0.0.0/8");
    let innerRan = false, inner = null;
    const evil = { toString() { innerRan = true; inner = p.contains("10.9.9.9"); return "10.1.1.1"; } };
    const outer = p.contains(evil);
    assert(innerRan, "toString is the coercion hook for Prefix.contains");
    eq(inner, true, "reentrant contains answers correctly");
    eq(outer, true, "outer contains answers for its own argument");
    eq(p.contains("11.0.0.1"), false, "Prefix intact after reentrant use");
}

{
    const TEXTS = ["", "a", "mississippi", "aaaa", "abababa",
                   "the quick brown fox jumps over the lazy dog",
                   "x".repeat(1000) + "needle" + "y".repeat(1000)];
    const PATS = ["a", "ss", "aa", "aba", "needle", "the", "zzz"];
    for (const algo of ["kmp", "bmh"]) {
        for (const pat of PATS) {
            const m = new Matcher(pat, { algo });
            eq(m.length, pat.length, `Matcher("${pat}").length`);
            for (const t of TEXTS) {
                eq(m.firstIn(t), t.indexOf(pat), `[${algo}] firstIn == index for ${JSON.stringify(pat)}`);
                eqArr(m.allIn(t), t.indexOfAll(pat), `[${algo}] allIn == indexOfAll`);
                eq(m.test(t), t.indexOf(pat) >= 0, `[${algo}] test agrees with index`);
                eq(m.countIn(t), m.allIn(t).length, `[${algo}] countIn == allIn.length`);
            }
        }
    }
    for (const pat of PATS)
        for (const t of TEXTS)
            eqArr(new Matcher(pat, { algo: "kmp" }).allIn(t),
                  new Matcher(pat, { algo: "bmh" }).allIn(t),
                  "kmp and bmh agree: " + JSON.stringify([pat, t.length]));
    {
        const alias = new Matcher("ss", { algo: "boyer-moore" });
        eqArr(alias.allIn("mississippi"),
              new Matcher("ss", { algo: "bmh" }).allIn("mississippi"),
              '"boyer-moore" is an alias for "bmh"');
        n++;
        if (alias.algo !== "boyer-moore")
            throw new Error("the alias must report back as boyer-moore, got " + alias.algo);
    }

    {
        n++;
        const big = new Matcher("z".repeat(4096));
        if (big.length !== 4096 || big.firstIn("q" + "z".repeat(4096)) !== 1)
            throw new Error("a long pattern must still search correctly");
    }
    for (const bad of ["KMP", "BMH", "rabin-karp", "", "kmp2"]) {
        n++;
        let threw = false;
        try { new Matcher("x", { algo: bad }); } catch (e) { threw = e instanceof RangeError; }
        if (!threw) throw new Error("unknown Matcher algo must throw RangeError: " + JSON.stringify(bad));
    }
    const m = new Matcher("ss");
    const base = m.allIn("mississippi");
    for (let i = 0; i < 200; i++) {
        m.allIn("aaaa"); m.firstIn(""); m.countIn("ssss");
        eqArr(m.allIn("mississippi"), base, "Matcher is pure across reuse");
    }
}

{
    const h = new Hasher("sha256");
    const first = h.update("abc").digestHex();
    eq(first, SHA256Hex("abc"), "streaming matches one-shot");
    const carried = h.update("def").digestHex();
    eq(carried, SHA256Hex("abcdef"), "without reset(), input accumulates");
    h.reset();
    eq(h.update("abc").digestHex(), first, "reset() restores the initial state");
}

print("test_capabilities: all " + n + " tests passed");
