/* bench_ext_all.js — measure the native ext methods (Number/Object/
 * Function) against hand-JS equivalents. RATIO = native/JS (<1 native wins).
 * Best-of-N warmed loops via performance.now(). Run: dynajs tests/bench_ext_all.js */

function bench(name, iters, fn) {
    for (let w = 0; w < 3; w++) fn(2000);       /* warm */
    let best = Infinity;
    for (let r = 0; r < 5; r++) {
        const t0 = performance.now();
        fn(iters);
        const dt = performance.now() - t0;
        if (dt < best) best = dt;
    }
    return best;
}
function row(name, iters, nativeFn, jsFn) {
    const nt = bench(name, iters, nativeFn);
    const jt = bench(name, iters, jsFn);
    const ratio = nt / jt;
    const tag = ratio < 0.98 ? "WIN " : ratio > 1.05 ? "LOSE" : "~=  ";
    print(tag + " " + name.padEnd(22) +
          " native " + nt.toFixed(2).padStart(8) +
          "  js " + jt.toFixed(2).padStart(8) +
          "  ratio " + ratio.toFixed(3));
}

print("=== Number.prototype ===");
row("round(2)", 2e6,
    (n) => { let s = 0; for (let i = 0; i < n; i++) s += (3.14159).round(2); return s; },
    (n) => { let s = 0; for (let i = 0; i < n; i++) s += Math.round(3.14159 * 100) / 100; return s; });
row("clamp", 2e6,
    (n) => { let s = 0; for (let i = 0; i < n; i++) s += (i % 200).clamp(10, 100); return s; },
    (n) => { let s = 0; for (let i = 0; i < n; i++) { let v = i % 200; s += v < 10 ? 10 : v > 100 ? 100 : v; } return s; });
row("isEven", 2e6,
    (n) => { let s = 0; for (let i = 0; i < n; i++) if ((i).isEven()) s++; return s; },
    (n) => { let s = 0; for (let i = 0; i < n; i++) if (Number.isInteger(i) && i % 2 === 0) s++; return s; });

print("=== Object static ===");
const obj = { a: 1, b: 2, c: 3, d: 4 };
row("pick", 5e5,
    (n) => { for (let i = 0; i < n; i++) Object.pick(["a", "c"], obj); },
    (n) => { for (let i = 0; i < n; i++) { const o = {}; for (const k of ["a", "c"]) if (k in obj) o[k] = obj[k]; } });
row("omit", 5e5,
    (n) => { for (let i = 0; i < n; i++) Object.omit(["b"], obj); },
    (n) => { for (let i = 0; i < n; i++) { const o = {}; for (const k in obj) if (k !== "b") o[k] = obj[k]; } });
row("merge/mergeRight", 5e5,
    (n) => { for (let i = 0; i < n; i++) Object.mergeRight(obj, { c: 9, e: 5 }); },
    (n) => { for (let i = 0; i < n; i++) Object.assign({}, obj, { c: 9, e: 5 }); });
const deepObj = { a: { b: { c: 1 }, d: [1, 2, 3] }, e: "x", f: 2 };
row("clone(deep)", 2e5,
    (n) => { for (let i = 0; i < n; i++) Object.clone(deepObj); },
    (n) => { for (let i = 0; i < n; i++) structuredCloneJS(deepObj); });
row("equals(deep)", 5e5,
    (n) => { let s = 0; for (let i = 0; i < n; i++) if (Object.equals(deepObj, deepObj)) s++; return s; },
    (n) => { let s = 0; for (let i = 0; i < n; i++) if (JSON.stringify(deepObj) === JSON.stringify(deepObj)) s++; return s; });
row("path(literal array)", 1e6,
    (n) => { let s = 0; for (let i = 0; i < n; i++) s += Object.path(["a", "b", "c"], deepObj); return s; },
    (n) => { let s = 0; for (let i = 0; i < n; i++) s += deepObj?.a?.b?.c; return s; });
{
    const P = ["a", "b", "c"];
    row("path(hoisted array)", 1e6,
        (n) => { let s = 0; for (let i = 0; i < n; i++) s += Object.path(P, deepObj); return s; },
        (n) => { let s = 0; for (let i = 0; i < n; i++) s += deepObj?.a?.b?.c; return s; });
}
row("path(dotted string)", 1e6,
    (n) => { let s = 0; for (let i = 0; i < n; i++) s += Object.path("a.b.c", deepObj); return s; },
    (n) => { let s = 0; for (let i = 0; i < n; i++) s += deepObj?.a?.b?.c; return s; });

function structuredCloneJS(o) {
    if (Array.isArray(o)) return o.map(structuredCloneJS);
    if (o && typeof o === "object") { const r = {}; for (const k in o) r[k] = structuredCloneJS(o[k]); return r; }
    return o;
}

print("=== Function combinators ===");
const inc = (x) => x + 1, dbl = (x) => x * 2, neg = (x) => -x;
row("pipe(build+call)", 1e6,
    (n) => { let s = 0; for (let i = 0; i < n; i++) s += inc.pipe(dbl, neg)(i); return s; },
    (n) => { let s = 0; for (let i = 0; i < n; i++) s += (x => neg(dbl(inc(x))))(i); return s; });
{
    const piped = inc.pipe(dbl, neg);
    const handrolled = (x) => neg(dbl(inc(x)));
    row("pipe(call only)", 3e6,
        (n) => { let s = 0; for (let i = 0; i < n; i++) s += piped(i); return s; },
        (n) => { let s = 0; for (let i = 0; i < n; i++) s += handrolled(i); return s; });
}
row("compose(call only)", 3e6,
    ((f) => (n) => { let s = 0; for (let i = 0; i < n; i++) s += f(i); return s; })(inc.compose(dbl)),
    ((f) => (n) => { let s = 0; for (let i = 0; i < n; i++) s += f(i); return s; })((x) => inc(dbl(x))));
{
    const add3 = (a, b, c) => a + b + c;
    const c3 = add3.curry();
    row("curry(call)", 5e5,
        (n) => { let s = 0; for (let i = 0; i < n; i++) s += c3(1)(2)(3); return s; },
        (n) => { let s = 0; for (let i = 0; i < n; i++) s += ((a) => (b) => (c) => a + b + c)(1)(2)(3); return s; });
}

print("done.");
