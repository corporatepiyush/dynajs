function gen(nvars, nfuncs) {
    let src = "let sink=0;\n";
    for (let f = 0; f < nfuncs; f++) {
        src += `function fn${f}(){\n`;
        for (let v = 0; v < nvars; v++) src += `  var v${v} = ${v};\n`;
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
