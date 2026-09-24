/* bench_scope_scaling.js -- is the scope-variable lookup superlinear?
 *
 * find_var and find_arg are backward linear scans, so define_var over a
 * function with n locals is O(n^2). This holds the TOTAL variable count fixed
 * at 8000 and varies only how many of them live in one function, so any rise
 * in cost-per-variable is the quadratic and nothing else.
 *
 * The answer decides whether a hash table in JSFunctionDef is worth its
 * complexity, and it is two answers rather than one: flat to about 100 locals
 * per function, and 2.55x by 1600. Hand-written code measures 2.57 entries
 * scanned per lookup, so it sits in the flat part; bundled output need not.
 */
function gen(nvars, nfuncs) {
    let src = "let sink=0;\n";
    for (let f = 0; f < nfuncs; f++) {
        src += `function fn${f}(){\n`;
        for (let v = 0; v < nvars; v++) src += `  var v${v} = ${v};\n`;
        /* every reference forces a lookup against the whole var list */
        for (let v = 0; v < nvars; v++) src += `  v${v} = v${(v + 1) % nvars} + 1;\n`;
        src += "  return v0;\n}\n";
    }
    return src + "sink;";
}
print("nvars  totalVars   ms    ns/var   ns/var normalised");
let base = 0;
for (const nvars of [25, 50, 100, 200, 400, 800, 1600]) {
    const nfuncs = Math.max(1, Math.round(8000 / nvars));
    const src = gen(nvars, nfuncs);
    const reps = 6;
    let best = Infinity;
    for (let r = 0; r < 3; r++) {
        const t0 = performance.now();
        for (let i = 0; i < reps; i++) eval(src);
        const dt = performance.now() - t0;
        if (dt < best) best = dt;
    }
    const totalVars = nvars * nfuncs * reps;
    const nsPerVar = best * 1e6 / totalVars;
    if (!base) base = nsPerVar;
    print(`${String(nvars).padStart(5)} ${String(nvars*nfuncs).padStart(9)} ${best.toFixed(0).padStart(6)} ${nsPerVar.toFixed(1).padStart(8)} ${(nsPerVar/base).toFixed(2).padStart(8)}x`);
}
