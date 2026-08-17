/* bench_capabilities.js -- the W0-G crossover gate (STDLIB_OOP_PLAN 1.6).
 *
 * A compiled capability trades a one-off construction cost for a cheaper
 * per-use cost. That is only a win past some N, and the honest number to
 * publish is the CROSSOVER: how many uses of one instance it takes to beat N
 * calls of the free function.
 *
 * So each case is measured at N = 1, 10, 100, 1000 uses, INCLUDING construction
 * in the capability's timing -- otherwise the comparison is rigged. The N=1 row
 * is kept permanently: it is the bypass-never-fires case, and CLAUDE.md section
 * 4 requires the adversarial direction to stay measured, not be quietly dropped
 * once it stops flattering the design.
 *
 * A capability whose crossover is worse than ~10 for a plausible workload does
 * not ship (the plan's rule); below that, the free function may be deleted.
 *
 * Emits `#DATA<TAB>case<TAB>N<TAB>free_ns<TAB>cap_ns<TAB>ratio`.
 */
import { Range, satisfies } from "dyna:semver";
import { Prefix, contains } from "dyna:net";
import { Matcher, MultiMatcher } from "dyna:matcher";
import { Hasher, SHA256Hex } from "dyna:hash";
import { Hmac, HMACHex } from "dyna:crypto";
import { Compressor, lz4Compress } from "dyna:compress";
import { Format, formatUnix } from "dyna:time";

/* The N grid decides the resolution of the answer, and 1/10/100/1000 was too
 * coarse: the cost model in tests/bench_capability_cost.js predicts semver.Range
 * crosses over at N=2, and a decade grid can only report "10". The small values
 * are where a capability is actually decided -- almost every real caller is
 * down here, not at 1000. */
const NS = [1, 2, 3, 5, 10, 100, 1000];

/* Time `fn` once per trial, best of `trials`. Each trial does the WHOLE unit of
 * work (construct + N uses, or N free calls), so construction is never
 * amortised away by the harness itself. Repetition scales inversely with N so
 * every measurement lands in a usable range. */
function timeBest(fn, trials) {
    let best = Infinity;
    for (let t = 0; t < trials; t++) {
        const t0 = performance.now();
        fn();
        const dt = performance.now() - t0;
        if (dt < best) best = dt;
    }
    return best * 1e6;   /* ns */
}

function crossover(name, N, freeUnit, capUnit) {
    /* outer repetition so even N=1 runs long enough to measure */
    const reps = Math.max(1, Math.ceil(20000 / N));
    const free = timeBest(() => { for (let i = 0; i < reps; i++) freeUnit(); }, 5) / reps;
    const cap  = timeBest(() => { for (let i = 0; i < reps; i++) capUnit(); }, 5) / reps;
    const ratio = cap / free;
    print(`${name}  N=${String(N).padStart(4)}  free ${(free/1000).toFixed(2).padStart(9)} us` +
          `  cap ${(cap/1000).toFixed(2).padStart(9)} us  ratio ${ratio.toFixed(3)}` +
          (ratio < 1 ? "  WIN" : ""));
    print(`#DATA\t${name}\t${N}\t${free.toFixed(1)}\t${cap.toFixed(1)}\t${ratio.toFixed(4)}`);
    return ratio;
}

function report(name, ratios) {
    let cross = null;
    for (let i = 0; i < NS.length; i++)
        if (ratios[i] < 1) { cross = NS[i]; break; }
    print(`>>> ${name}: crossover at N=${cross === null ? ">1000 (DOES NOT PAY)" : cross}`);
    print("");
}

/* ---- semver Range ---- */
{
    const RS = ">=1.2.3 <2.0.0 || ^3.0.0";
    const V = "1.5.0";
    const ratios = NS.map(N => crossover("semver.Range", N,
        () => { for (let i = 0; i < N; i++) satisfies(V, RS); },
        () => { const r = new Range(RS); for (let i = 0; i < N; i++) r.test(V); }));
    report("semver.Range", ratios);
}

/* ---- netip Prefix ---- */
{
    const P = "10.0.0.0/8", A = "10.1.2.3";
    const ratios = NS.map(N => crossover("netip.Prefix", N,
        () => { for (let i = 0; i < N; i++) contains(P, A); },
        () => { const p = new Prefix(P); for (let i = 0; i < N; i++) p.contains(A); }));
    report("netip.Prefix", ratios);
}

/* ---- matcher Matcher, on a SHORT text ----
 * The adversarial case: the plain search wins for a
 * single use because Matcher has no table to build. Kept so that admission
 * stays measured.
 *
 * NOTE the baseline changed with W2. It used to be dyna:strings' index(), a
 * native call taking two strings to coerce; now that module is gone and the
 * function a caller would otherwise reach for is String.prototype.indexOf,
 * which the engine can call far more cheaply. Comparing against the thing a
 * caller would ACTUALLY write is the only comparison worth publishing, and it
 * moved the long-text answer from N=100 to N=1. */
{
    const PAT = "needle", TEXT = "haystack with a needle in it";
    const ratios = NS.map(N => crossover("matcher.Matcher/short", N,
        () => { for (let i = 0; i < N; i++) TEXT.indexOf(PAT); },
        () => { const m = new Matcher(PAT); for (let i = 0; i < N; i++) m.firstIn(TEXT); }));
    report("matcher.Matcher/short", ratios);
}

/* ---- matcher Matcher, on a LONG text (the case it is for) ---- */
{
    const PAT = "needle", TEXT = "x".repeat(20000) + "needle" + "y".repeat(20000);
    const ratios = NS.map(N => crossover("matcher.Matcher/long", N,
        () => { for (let i = 0; i < N; i++) TEXT.indexOf(PAT); },
        () => { const m = new Matcher(PAT); for (let i = 0; i < N; i++) m.firstIn(TEXT); }));
    report("matcher.Matcher/long", ratios);
}

/* ---- matcher MultiMatcher: the capability Matcher only looks like ----
 *
 * The free-function equivalent of "find any of these N patterns" is N separate
 * searches, i.e. N passes over the text. The automaton does ONE, and its cost
 * does not grow with N. This is the row that justifies the class -- and it is
 * measured against the honest alternative, not against a strawman. */
{
    const PATS = ["ERROR", "WARN", "FATAL", "panic:", "Traceback", "OOM",
                  "segfault", "assertion"];
    const TEXT = ("2026-07-27T10:00:00Z INFO [worker-3] request id=42 " +
                  "path=/api/v1/resource status=200 dur=17ms\n").repeat(20) +
                 "2026-07-27T10:00:01Z ERROR [worker-3] panic: bad\n";
    const ratios = NS.map(N => crossover("matcher.MultiMatcher", N,
        () => { for (let i = 0; i < N; i++)
                    for (const p of PATS) TEXT.indexOf(p); },
        () => { const mm = new MultiMatcher(PATS);
                for (let i = 0; i < N; i++) mm.firstIn(TEXT); }));
    report("matcher.MultiMatcher", ratios);
}

/* ---- time Format ----
 * The one capability in this file whose verdict was PREDICTED before it was
 * built: tests/bench_time_layout.js measured the layout scan at 46.6% of a
 * formatUnix call, and tests/bench_capability_cost.js's model
 * (pays iff configParse > (K-1) x 20.3 ns, crossover ctorCost/configParse)
 * put the crossover at ~2. Kept here so the prediction stays checkable. */
{
    const LAYOUT = "2006-01-02T15:04:05Z", SEC = 1735689600;
    const ratios = NS.map(N => crossover("time.Format", N,
        () => { for (let i = 0; i < N; i++) formatUnix(SEC, LAYOUT); },
        () => { const f = new Format(LAYOUT); for (let i = 0; i < N; i++) f.format(SEC); }));
    report("time.Format", ratios);
}

/* ---- crypto Hasher (reset+update+digest vs the one-shot) ---- */
{
    const MSG = "the quick brown fox jumps over the lazy dog";
    const ratios = NS.map(N => crossover("crypto.Hasher", N,
        () => { for (let i = 0; i < N; i++) SHA256Hex(MSG); },
        () => { const h = new Hasher("sha256");
                for (let i = 0; i < N; i++) { h.reset(); h.update(MSG); h.digestHex(); } }));
    report("crypto.Hasher", ratios);
}

/* ---- crypto Hmac (the key IS the configuration) ----
 *
 * The contrast with Hasher above is the point of the whole chapter. Hasher has
 * no configuration to hoist, so it starts behind and stays there. Hmac derives
 * a block-sized key schedule -- real per-call work in the free function -- and
 * replaces one call with one call, so K=1 and the cost model predicts it pays
 * almost immediately. */
{
    const KEY = "a-reasonably-long-secret-key-value";
    const MSG = "the quick brown fox jumps over the lazy dog";
    const ratios = NS.map(N => crossover("crypto.Hmac/shortkey", N,
        () => { for (let i = 0; i < N; i++) HMACHex("sha256", KEY, MSG); },
        () => { const m = new Hmac("sha256", KEY);
                for (let i = 0; i < N; i++) m.signHex(MSG);
                m.close(); }));
    report("crypto.Hmac/shortkey", ratios);
}

/* The same class with a key LONGER than the hash block, which the free function
 * must hash on every call rather than merely pad. That is the configuration the
 * constructor actually hoists, and separating the two cases is the difference
 * between "does not pay" and "pays at N=3". */
{
    const KEY = "k".repeat(200);          /* > SHA-256's 64-byte block */
    const MSG = "the quick brown fox jumps over the lazy dog";
    const ratios = NS.map(N => crossover("crypto.Hmac/longkey", N,
        () => { for (let i = 0; i < N; i++) HMACHex("sha256", KEY, MSG); },
        () => { const m = new Hmac("sha256", KEY);
                for (let i = 0; i < N; i++) m.signHex(MSG);
                m.close(); }));
    report("crypto.Hmac/longkey", ratios);
}

/* ---- compress Compressor (the match-finder scratch) ----
 * The full curve lives in tests/bench_compress.js; this row keeps it in the
 * one table a reader compares capabilities across. */
{
    const REC = JSON.stringify({ jsonrpc: "2.0", id: 7, method: "subscribe",
                                 params: { channel: "trades", symbol: "SYM7" } });
    const ratios = NS.map(N => crossover("compress.Compressor", N,
        () => { for (let i = 0; i < N; i++) lz4Compress(REC); },
        () => { const c = new Compressor({ algo: "lz4" });
                for (let i = 0; i < N; i++) c.compress(REC);
                c.close(); }));
    report("compress.Compressor", ratios);
}
