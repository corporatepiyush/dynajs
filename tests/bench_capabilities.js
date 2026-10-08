import { Range, satisfies } from "dyna:semver";
import { Prefix, contains } from "dyna:net";
import { Matcher, MultiMatcher } from "dyna:matcher";
import { Hasher, SHA256Hex } from "dyna:hash";
import { Hmac, HMACHex } from "dyna:crypto";
import { Compressor, lz4Compress } from "dyna:compress";
import { Format, formatUnix } from "dyna:time";

const NS = [1, 2, 3, 5, 10, 100, 1000];

function timeBest(fn, trials) {
    let best = Infinity;
    for (let t = 0; t < trials; t++) {
        const t0 = performance.now();
        fn();
        const dt = performance.now() - t0;
        if (dt < best) best = dt;
    }
    return best * 1e6;
}

function crossover(name, N, freeUnit, capUnit) {
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

{
    const RS = ">=1.2.3 <2.0.0 || ^3.0.0";
    const V = "1.5.0";
    const ratios = NS.map(N => crossover("semver.Range", N,
        () => { for (let i = 0; i < N; i++) satisfies(V, RS); },
        () => { const r = new Range(RS); for (let i = 0; i < N; i++) r.test(V); }));
    report("semver.Range", ratios);
}

{
    const P = "10.0.0.0/8", A = "10.1.2.3";
    const ratios = NS.map(N => crossover("netip.Prefix", N,
        () => { for (let i = 0; i < N; i++) contains(P, A); },
        () => { const p = new Prefix(P); for (let i = 0; i < N; i++) p.contains(A); }));
    report("netip.Prefix", ratios);
}

{
    const PAT = "needle", TEXT = "haystack with a needle in it";
    const ratios = NS.map(N => crossover("matcher.Matcher/short", N,
        () => { for (let i = 0; i < N; i++) TEXT.indexOf(PAT); },
        () => { const m = new Matcher(PAT); for (let i = 0; i < N; i++) m.firstIn(TEXT); }));
    report("matcher.Matcher/short", ratios);
}

{
    const PAT = "needle", TEXT = "x".repeat(20000) + "needle" + "y".repeat(20000);
    const ratios = NS.map(N => crossover("matcher.Matcher/long", N,
        () => { for (let i = 0; i < N; i++) TEXT.indexOf(PAT); },
        () => { const m = new Matcher(PAT); for (let i = 0; i < N; i++) m.firstIn(TEXT); }));
    report("matcher.Matcher/long", ratios);
}

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

{
    const LAYOUT = "2006-01-02T15:04:05Z", SEC = 1735689600;
    const ratios = NS.map(N => crossover("time.Format", N,
        () => { for (let i = 0; i < N; i++) formatUnix(SEC, LAYOUT); },
        () => { const f = new Format(LAYOUT); for (let i = 0; i < N; i++) f.format(SEC); }));
    report("time.Format", ratios);
}

{
    const MSG = "the quick brown fox jumps over the lazy dog";
    const ratios = NS.map(N => crossover("crypto.Hasher", N,
        () => { for (let i = 0; i < N; i++) SHA256Hex(MSG); },
        () => { const h = new Hasher("sha256");
                for (let i = 0; i < N; i++) { h.reset(); h.update(MSG); h.digestHex(); } }));
    report("crypto.Hasher", ratios);
}

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

{
    const KEY = "k".repeat(200);
    const MSG = "the quick brown fox jumps over the lazy dog";
    const ratios = NS.map(N => crossover("crypto.Hmac/longkey", N,
        () => { for (let i = 0; i < N; i++) HMACHex("sha256", KEY, MSG); },
        () => { const m = new Hmac("sha256", KEY);
                for (let i = 0; i < N; i++) m.signHex(MSG);
                m.close(); }));
    report("crypto.Hmac/longkey", ratios);
}

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
