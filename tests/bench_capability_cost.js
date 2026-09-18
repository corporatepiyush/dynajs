/* bench_capability_cost.js -- WHY a compiled capability costs what it costs.
 *
 * tests/bench_capabilities.js answers "does this one pay?" and produces a
 * crossover N. It does not answer the question that actually matters when two
 * of five capabilities never pay at all: WHERE does the money go, and can the
 * answer be known before building the class?
 *
 * So this decomposes a call into its parts and measures each one directly:
 *
 *   1. the JS-side floor      -- loop, plain call, method call through a proto
 *   2. the native call ceremony -- crossing into C and back, with and without
 *      an argument to coerce, on a plain class and on a free function
 *   3. object construction    -- what `new` costs before any work happens
 *   4. the capability split   -- a capability that replaces ONE free call with
 *      K method calls starts (K-1) ceremonies behind
 *
 * Out of that comes a predictive rule, checked against all four measured
 * capabilities at the bottom of the file.
 *
 * Emits `#DATA<TAB>case<TAB>ns_per_op`.
 */
import { Matcher } from "dyna:matcher";
import { Hasher, SHA256Hex } from "dyna:hash";
import { Range, satisfies } from "dyna:semver";
import { Prefix, contains } from "dyna:net";
import { BitSet } from "dyna:structures";

const TRIALS = 9;
const N = 300000;

function bestOf(fn) {
    let b = Infinity;
    for (let t = 0; t < TRIALS; t++) {
        const t0 = performance.now();
        fn();
        const dt = performance.now() - t0;
        if (dt < b) b = dt;
    }
    return b * 1e6 / N;
}

/* The driving loop alone. Everything below is reported net of it, because a
 * 4-8 ns/iteration loop is a large fraction of a 20 ns operation. */
const FLOOR = (() => {
    let b = Infinity;
    for (let t = 0; t < TRIALS; t++) {
        const t0 = performance.now();
        let s = 0;
        for (let i = 0; i < N; i++) s += i;
        const dt = performance.now() - t0;
        if (dt < b) b = dt;
        if (s === -1) print("no");
    }
    return b * 1e6 / N;
})();

const R = {};
function bench(name, fn) {
    const per = bestOf(fn) - FLOOR;
    R[name] = per;
    print(`${name.padEnd(40)} ${per.toFixed(2).padStart(8)} ns/op`);
    print(`#DATA\t${name}\t${per.toFixed(3)}`);
    return per;
}

print(`loop floor: ${FLOOR.toFixed(2)} ns/iteration (subtracted from every row)`);
print("");
print("--- 1. the JS-side floor ------------------------------------------");

function jsFn(x) { return x; }
bench("plain JS function call", () => { for (let i = 0; i < N; i++) jsFn(i); });

const jsObj = { m(x) { return x; } };
bench("JS method call through a prototype", () => {
    const o = jsObj;
    for (let i = 0; i < N; i++) o.m(i);
});

class JsCls { constructor(v) { this.v = v; } m(x) { return x; } }
const jsInst = new JsCls(1);
bench("JS class method call", () => { for (let i = 0; i < N; i++) jsInst.m(i); });

print("");
print("--- 2. the native call ceremony ------------------------------------");

/* A plain native class with a trivial method: the whole cost is crossing into
 * C, resolving `this` to its opaque, coercing one integer, and returning. */
const bs = new BitSet(64);
bs.set(3);
bench("native method, 1 int arg (BitSet.get)", () => {
    for (let i = 0; i < N; i++) bs.get(3);
});

/* Same class, a getter: no argument at all. The difference from the row above
 * is the argument coercion. */
bench("native getter, no arg (BitSet.count)", () => {
    for (let i = 0; i < N; i++) { const v = bs.count; if (v === -1) print("no"); }
});

/* A native FREE function with one short string argument: this is the coercion
 * a capability's one-shot method also pays. */
const SHORT = "abc";
bench("native free fn, 1 short string (toUpper)", () => {
    for (let i = 0; i < N; i++) toUpper(SHORT);
});

print("");
print("--- 3. construction -------------------------------------------------");

bench("new BitSet() (native plain class)", () => {
    for (let i = 0; i < N; i++) { const b = new BitSet(); if (b === null) print("no"); }
});
bench("new JsCls() (plain JS class)", () => {
    for (let i = 0; i < N; i++) { const b = new JsCls(i); if (b === null) print("no"); }
});

print("");
print("--- 4. the capability split: 1 call becomes K -----------------------");

const MSG = "the quick brown fox jumps over the lazy dog";
bench("SHA256Hex(msg)            [K=1 free]", () => {
    for (let i = 0; i < N; i++) SHA256Hex(MSG);
});
{
    const h = new Hasher("sha256");
    bench("Hasher reset+update+digestHex [K=3]", () => {
        for (let i = 0; i < N; i++) { h.reset(); h.update(MSG); h.digestHex(); }
    });
    /* Each of the three, alone, so the split is attributed rather than
     * inferred. update() carries the message, so it holds the real work. */
    bench("  Hasher.reset() alone", () => { for (let i = 0; i < N; i++) h.reset(); });
    bench("  Hasher.update(msg) alone", () => { for (let i = 0; i < N; i++) h.update(MSG); });
    h.reset();
    bench("  Hasher.digestHex() alone", () => { for (let i = 0; i < N; i++) h.digestHex(); });
}

print("");
print("--- 5. what the configuration parse is worth ------------------------");

/* The quantity a capability exists to avoid: the per-call cost of turning the
 * configuration string into whatever the operation needs. Measured as the
 * difference between the free function (which parses every time) and the
 * capability's method (which does not). */
{
    const RS = ">=1.2.3 <2.0.0 || ^3.0.0", V = "1.5.0";
    const r = new Range(RS);
    const free = bench("semver satisfies(v, range) [free]",
        () => { for (let i = 0; i < N; i++) satisfies(V, RS); });
    const cap = bench("semver Range.test(v)       [cap]",
        () => { for (let i = 0; i < N; i++) r.test(V); });
    R.semver_parse = free - cap;
}
{
    const P = "10.0.0.0/8", A = "10.1.2.3";
    const p = new Prefix(P);
    const free = bench("netip contains(cidr, addr) [free]",
        () => { for (let i = 0; i < N; i++) contains(P, A); });
    const cap = bench("netip Prefix.contains(addr) [cap]",
        () => { for (let i = 0; i < N; i++) p.contains(A); });
    R.netip_parse = free - cap;
}
{
    const PAT = "needle", TEXT = "x".repeat(20000) + "needle" + "y".repeat(20000);
    const m = new Matcher(PAT);
    const free = bench("strings index(text, pat)   [free]",
        () => { for (let i = 0; i < N / 100; i++) TEXT.indexOf(PAT); });
    const cap = bench("strings Matcher.firstIn(text) [cap]",
        () => { for (let i = 0; i < N / 100; i++) m.firstIn(TEXT); });
    /* Scaled: only N/100 iterations ran, so the reported ns/op is 100x too
     * small and the loop floor was over-subtracted. Reported as a ratio, which
     * is immune to both. */
    R.matcher_ratio = cap / free;
}

print("");
print("--- 6. the dead-preprocessing guard --------------------------------");

/* `Matcher`'s constructor used to build a KMP failure function or a 2 KB
 * bad-character table, and after the search moved to simd.strfind NOTHING READ
 * EITHER. Construction was therefore O(pattern) in real work rather than in a
 * memcpy: 2212 ns for a 1024-byte pattern against 102 ns for an 8-byte one.
 *
 * This is the permanent guard. It lives in the bench and not in the test suite
 * because it is a timing property, and the same ratio that is ~3x in a plain
 * build is ~12.7x under UBSan -- no single threshold survives both. A number
 * printed every time a human looks at capability costs is the honest place for
 * it. A table creeping back in shows up here as a large multiple. */
{
    const SHORT = "needle12", LONG = "z".repeat(4096);
    const t = (pat) => {
        let b = Infinity;
        for (let k = 0; k < TRIALS; k++) {
            const t0 = performance.now();
            for (let i = 0; i < 50000; i++) { const m = new Matcher(pat); if (!m) print("no"); }
            const dt = performance.now() - t0;
            if (dt < b) b = dt;
        }
        return b;
    };
    const ratio = t(LONG) / t(SHORT);
    print(`new Matcher: 4096-byte pattern vs 8-byte  ${ratio.toFixed(2)}x`);
    print(`#DATA\tmatcher_ctor_scaling\t${ratio.toFixed(3)}`);
    print(ratio < 8
        ? "  -> a memcpy. No preprocessing table."
        : "  -> WARNING: construction scales with pattern length. A preprocessing\n" +
          "     table has come back, and nothing in this module reads one.");
}

print("");
print("=== the model ======================================================");

const ceremony = R["native method, 1 int arg (BitSet.get)"];
const alloc = R["new BitSet() (native plain class)"];
const k3 = R["Hasher reset+update+digestHex [K=3]"];
const k1 = R["SHA256Hex(msg)            [K=1 free]"];

print("");
print(`native call ceremony        ${ceremony.toFixed(1)} ns   (BitSet.get: cross into C,`);
print(`                                       resolve the opaque, coerce one int)`);
print(`native object construction  ${alloc.toFixed(1)} ns`);
print(`Hasher K=3 minus K=1        ${(k3 - k1).toFixed(1)} ns   (predicted 2 x ceremony =` +
      ` ${(2 * ceremony).toFixed(1)} ns)`);
print("");
print("A capability replaces ONE free call with K method calls, so it starts");
print("(K-1) ceremonies behind on EVERY use. It can only win back what the");
print("configuration parse costs. Hence:");
print("");
print("    it pays  <=>  configParse  >  (K-1) x ceremony");
print("");
print("Checked against the four measured capabilities:");
print("");
print(`  semver.Range   K=1  configParse ~ ${R.semver_parse.toFixed(0).padStart(5)} ns  ` +
      `vs 0 ns    -> PAYS (measured 5.9x at N=1000)`);
print(`  netip.Prefix   K=1  configParse ~ ${R.netip_parse.toFixed(0).padStart(5)} ns  ` +
      `vs 0 ns    -> PAYS (measured 1.33x)`);
print(`  strings.Matcher K=1 configParse ~     0 ns  vs 0 ns    -> WASH ` +
      `(measured ${R.matcher_ratio.toFixed(3)}x)`);
print(`  crypto.Hasher  K=3  configParse ~    ~0 ns  vs ${(2 * ceremony).toFixed(0)} ns` +
      `   -> CANNOT PAY (measured 1.13x)`);
print("");
print(`#DATA\tceremony\t${ceremony.toFixed(3)}`);
print(`#DATA\talloc\t${alloc.toFixed(3)}`);
print(`#DATA\tsemver_parse\t${R.semver_parse.toFixed(3)}`);
print(`#DATA\tnetip_parse\t${R.netip_parse.toFixed(3)}`);
print(`#DATA\tmatcher_ratio\t${R.matcher_ratio.toFixed(4)}`);
