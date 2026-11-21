// covers: c:src/value/atoms.inc.c
// F-opts-1: natives read option objects with JS_GetPropertyStr(ctx, opts, "name"), which hashed the C string into an
// atom on every call; a name that no live object or source held was allocated as a fresh atom and freed again each
// call (oauth2.buildAuthorizationUrl reads ~10 option names). JS_GetPropertyStr, JS_SetPropertyStr and
// JS_DefinePropertyValueStr now go through a 64-entry runtime cache keyed by the C string's address, verified
// against the cached atom's characters (a reused buffer with new contents misses), holding one reference so the atom
// stays interned. buildAuthorizationUrl with every optional name absent: 790 -> 619 ns; with them interned: 703 ->
// 620 ns; with PKCE + extraParams: 1229 -> 1103 ns; parseAuthorizationResponse (result keys defined by name)
// 233 -> 228 ns; new SAXParser(undefined, opts) + one event 1326-1411 -> 1283-1320 ns (noisy); JSON_OBJECT_AGG over 100K fractional keys (every
// lookup a cache miss) 31.0 -> 31.2 ms, within noise (CPU best of 7, load 5-10).
// Oracles: the cache never changes what is read (a Proxy sees the same get traps on every call, a getter runs on
// every call), a native that reuses one stack buffer for different keys (JSON_OBJECT_AGG on a fractional key column)
// keeps every key distinct across more keys than the cache holds (the keys are held by another object first: Set/Define
// only cache atoms that already exist, so they never extend an atom's life -- test_parse_dup_hash_leak counts blocks); perf: absent option names cost the same as
// interned ones (old: 1.12x).
import { suite } from "../lib/kit.js";
import * as oauth2 from "dyna:oauth2";
import { DataFrame } from "dyna:dataframe";
import { cpuNow } from "../lib/cpuclock.js";
import { perfCheck } from "../lib/perf.js";

const t = suite("test_core_str_atom_cache");
const base = { authorizationEndpoint: "https://auth.example.com/authorize", clientId: "client-123", redirectUri: "https://app.example.com/cb", state: "st-abc", scope: "openid profile" };

t.test("absent option names cost the same as interned ones", ({ ok, skip }) => {
    const N = 20000;
    const time = () => {
        let best = Infinity, x = 0;
        for (let k = 0; k < 9; k++) {
            const t0 = cpuNow();
            for (let i = 0; i < N; i++) x += oauth2.buildAuthorizationUrl(base).url.length;
            best = Math.min(best, cpuNow() - t0);
        }
        ok(x > 0, "urls built");
        return best;
    };
    const absent = time();
    const keep = {};
    for (const p of [["allow", "Insecure"], ["allow", "Plain"], ["response", "Type"], ["extra", "Params"], ["code", "Challenge"], ["code", "ChallengeMethod"]]) keep[p.join("")] = 0;
    const interned = time();
    ok(Object.keys(keep).length === 6, "names interned while timing");
    perfCheck(ok, absent < interned * 1.06, "absent/interned option names: " + (absent / interned).toFixed(3) + "x (uncached: 1.12x)", skip);
});

t.test("a Proxy sees the same option reads on every call", ({ deep, eq }) => {
    const seen = [];
    const p = new Proxy({ ...base }, { get(o, k, r) { seen.push(k); return Reflect.get(o, k, r); } });
    const runs = [];
    for (let i = 0; i < 3; i++) { seen.length = 0; oauth2.buildAuthorizationUrl(p); runs.push(seen.slice()); }
    eq(runs[0].length > 5, true, "options read through the trap");
    deep(runs[1], runs[0], "second call reads the same names");
    deep(runs[2], runs[0], "third call reads the same names");
});

t.test("a getter runs on every call", ({ eq }) => {
    let n = 0;
    const o = { ...base, get state() { n++; return "s" + n; } };
    const a = oauth2.buildAuthorizationUrl(o).state, b = oauth2.buildAuthorizationUrl(o).state;
    eq(a, "s1", "first read");
    eq(b, "s2", "second read is not cached");
});

t.test("one reused key buffer keeps keys distinct past the cache size", ({ eq, deep }) => {
    const n = 300, ks = new Float64Array(n), vs = new Float64Array(n);
    const held = {};
    for (let i = 0; i < n; i++) { ks[i] = i + 0.5; vs[i] = i; held[String(i + 0.5)] = i; }
    for (let round = 0; round < 3; round++) {
        const o = JSON.parse(new DataFrame({ k: ks, v: vs }).JSON_OBJECT_AGG("k", "v"));
        eq(Object.keys(o).length, n, "round " + round + ": every key distinct");
        eq(o["0.5"], 0, "round " + round + ": first key");
        eq(o["299.5"], 299, "round " + round + ": last key");
    }
    const small = JSON.parse(new DataFrame({ k: Float64Array.from([1.5, 2.5, 1.5]), v: Float64Array.from([1, 2, 3]) }).JSON_OBJECT_AGG("k", "v"));
    deep(small, { "1.5": 3, "2.5": 2 }, "last value of a duplicate key");
});

await t.run();
