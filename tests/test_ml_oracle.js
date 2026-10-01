// flags: --std
import {
    LinearRegression, LogisticRegression, KMeans,
    DecisionTreeClassifier, DecisionTreeRegressor,
    RandomForestClassifier, RandomForestRegressor,
    GradientBoostingClassifier, GradientBoostingRegressor,
    GaussianNB, KNClassifier, PCA, StandardScaler, SVC,
} from "dyna:ml";
import * as std from "std";

const args = scriptArgs.slice(1);
if (args[0] === "--diff") {
    const a = std.open(args[1], "r").readAsString().trim().split("\n");
    const b = std.open(args[2], "r").readAsString().trim().split("\n");
    if (a.length !== b.length)
        throw new Error(`line count differs: ${a.length} vs ${b.length}`);
    const RTOL = 1e-6, ATOL = 1e-12;
    let checked = 0, exact = 0, drifted = 0, maxRel = 0, maxAbs = 0, worst = "";
    for (let i = 0; i < a.length; i++) {
        const [ka, ...va] = a[i].split(" ");
        const [kb, ...vb] = b[i].split(" ");
        if (ka !== kb) throw new Error(`key mismatch at line ${i + 1}: ${ka} vs ${kb}`);
        if (va.length !== vb.length)
            throw new Error(`field count mismatch at ${ka}`);
        const isLabel = ka.includes("labels") || ka.includes("classes");
        for (let j = 0; j < va.length; j++) {
            checked++;
            if (va[j] === vb[j]) { exact++; continue; }
            if (isLabel)
                throw new Error(`LABEL DISAGREEMENT at ${ka}[${j}]: ${va[j]} vs ${vb[j]}`);
            const x = parseFloat(va[j]), y = parseFloat(vb[j]);
            const abs = Math.abs(x - y);
            const rel = abs / Math.max(1e-300, Math.abs(y));
            if (abs > maxAbs) maxAbs = abs;
            if (rel > maxRel) { maxRel = rel; worst = `${ka}[${j}] ${x} vs ${y}`; }
            if (abs > ATOL && rel > RTOL)
                throw new Error(`TOLERANCE EXCEEDED at ${ka}[${j}]: ${x} vs ${y} ` +
                                `(abs ${abs.toExponential(3)}, rel ${rel.toExponential(3)})`);
            drifted++;
        }
    }
    print(`oracle diff: ${a.length} rows, ${checked} values, ` +
          `${exact} bit-identical, ${drifted} within tolerance ` +
          `(atol ${ATOL}, rtol ${RTOL})`);
    print(`worst drift: abs ${maxAbs.toExponential(3)}, rel ${maxRel.toExponential(3)}` +
          (worst ? "  (" + worst + ")" : ""));
    print("test_ml_oracle: SIMD and scalar builds agree");
} else {
    function rng(seed) {
        let s = seed >>> 0;
        return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
    }
    const f = (v) => v.toPrecision(17);
    function emit(key, values) { print(key + " " + values.map(f).join(" ")); }

    const SHAPES = [];
    for (const seed of [0, 1, 2])
        for (const [r, c] of [[40, 1], [40, 3], [60, 7], [60, 8], [80, 9],
                              [200, 16], [200, 33], [400, 64], [300, 129]])
            SHAPES.push([r, c, seed]);

    for (const [rows, cols, seed] of SHAPES) {
        const tag = `${rows}x${cols}s${seed}`;
        const rnd = rng(1000 + rows * 31 + cols + seed * 7919);
        const X = new Float64Array(rows * cols);
        const y = new Float64Array(rows);
        const w = new Float64Array(cols);
        for (let j = 0; j < cols; j++) w[j] = rnd() * 4 - 2;
        for (let i = 0; i < rows; i++) {
            let acc = 1.25;
            for (let j = 0; j < cols; j++) {
                const v = (rnd() * 2 - 1) * 100;
                X[i * cols + j] = v;
                acc += w[j] * v;
            }
            y[i] = acc + (rnd() - 0.5);
        }

        {
            const m = new LinearRegression();
            try {
                m.fit(X, y, rows, cols);
                const p = m.predict(X, rows, cols);
                emit(`linreg.${tag}.predict`, p.slice(0, 12));
                emit(`linreg.${tag}.predict_tail`, p.slice(-4));
            } finally { m.close(); }
        }

        {
            const cls = new Float64Array(rows);
            for (let i = 0; i < rows; i++) cls[i] = y[i] > 1.25 ? 1 : 0;
            const m = new LogisticRegression();
            try {
                m.fit(X, cls, rows, cols);
                emit(`logreg.${tag}.proba`,
                     m.predictProba(X, rows, cols).slice(0, 12).map((r) => r[1]));
                emit(`logreg.${tag}.classes`, m.predict(X, rows, cols));
            } finally { m.close(); }
        }
    }

    const KCASES = [];
    for (const seed of [0, 1, 2])
        for (const [r, c, k, spread] of [[120, 2, 3, 0.5], [120, 8, 4, 0.5],
                                         [200, 16, 5, 0.5], [200, 33, 4, 0.5],
                                         [300, 64, 6, 0.5], [150, 129, 3, 0.5],
                                         [200, 16, 5, 8.0], [200, 9, 4, 12.0]])
            KCASES.push([r, c, k, spread, seed]);

    for (const [rows, cols, k, spread, seed] of KCASES) {
        const rnd = rng(7000 + rows + cols * 13 + k + seed * 6151);
        const X = new Float64Array(rows * cols);
        const centres = new Float64Array(k * cols);
        for (let c = 0; c < k * cols; c++) centres[c] = rnd() * 200 - 100;
        for (let i = 0; i < rows; i++) {
            const c = i % k;
            for (let j = 0; j < cols; j++)
                X[i * cols + j] = centres[c * cols + j] + (rnd() - 0.5) * spread;
        }
        const m = new KMeans(k, 12345 + seed);
        try {
            m.fit(X, rows, cols);
            emit(`kmeans.${rows}x${cols}k${k}sp${spread}s${seed}.labels`, m.predict(X, rows, cols));
            emit(`kmeans.${rows}x${cols}k${k}sp${spread}s${seed}.inertia`, [m.inertia]);
        } finally { m.close(); }
    }
}

{
    const rng2 = (seed) => { let st = seed >>> 0;
        return () => { st = (st * 1664525 + 1013904223) >>> 0; return st / 4294967296; }; };
    const fmt = (v) => Number(v).toPrecision(17);
    const emit = (key, values) =>
        print(key + " " + Array.from(values).map(fmt).join(" "));

    const rnd = rng2(777);
    const ROWS = 240, COLS = 5;
    const Xr = [], yc = [], yr = [];
    for (let i = 0; i < ROWS; i++) {
        const row = [];
        for (let j = 0; j < COLS; j++) row.push(rnd() * 4 - 2);
        const sig = row[0] * 1.5 - row[1] + row[2] * 0.5;
        Xr.push(row);
        yc.push(sig + (rnd() - 0.5) * 0.8 > 0 ? 1 : 0);
        yr.push(sig + (rnd() - 0.5) * 0.2);
    }
    const probe = Xr.slice(0, 10);

    const dump = (name, m, isClf) => {
        emit(`${name}.predict`, m.predict(probe));
        if (isClf && typeof m.predictProba === "function") {
            const P = m.predictProba(probe);
            for (let i = 0; i < P.length; i++) emit(`${name}.proba${i}`, P[i]);
        }
        if (m.featureImportances !== undefined)
            emit(`${name}.importances`, m.featureImportances);
        if (typeof m.apply === "function") emit(`${name}.leaves`, m.apply(probe)[0]);
    };

    for (const depth of [3, 6]) {
        let m = new DecisionTreeClassifier({ maxDepth: depth, seed: 1 });
        try { m.fit(Xr, yc); dump(`dtc.d${depth}`, m, 1); } finally { m.close(); }
        m = new DecisionTreeRegressor({ maxDepth: depth, seed: 1 });
        try { m.fit(Xr, yr); dump(`dtr.d${depth}`, m, 0); } finally { m.close(); }
    }
    for (const n of [8, 25]) {
        let m = new RandomForestClassifier({ nEstimators: n, maxDepth: 5, seed: 7 });
        try { m.fit(Xr, yc); dump(`rfc.n${n}`, m, 1); } finally { m.close(); }
        m = new RandomForestRegressor({ nEstimators: n, maxDepth: 5, seed: 7 });
        try { m.fit(Xr, yr); dump(`rfr.n${n}`, m, 0); } finally { m.close(); }
    }
    for (const n of [5, 15]) {
        let m = new GradientBoostingClassifier({ nEstimators: n, maxDepth: 3, seed: 3 });
        try { m.fit(Xr, yc); dump(`gbc.n${n}`, m, 1); } finally { m.close(); }
        m = new GradientBoostingRegressor({ nEstimators: n, maxDepth: 3, seed: 3 });
        try { m.fit(Xr, yr); dump(`gbr.n${n}`, m, 0); } finally { m.close(); }
    }

    { const m = new GaussianNB();
      try { m.fit(Xr, yc); dump("nb", m, 1); } finally { m.close(); } }
    { const m = new KNClassifier(5);
      try { m.fit(Xr, yc); dump("knn.k5", m, 1); } finally { m.close(); } }
    { const m = new SVC({ maxIter: 200 });
      try { m.fit(Xr, yc); dump("svc", m, 1); } finally { m.close(); } }
    { const m = new PCA(3);
      try { m.fit(Xr); emit("pca.transform0", m.transform(probe)[0]);
            emit("pca.explained", m.explainedVariance);
            emit("pca.mean", m.mean);
      } finally { m.close(); } }
    { const m = new StandardScaler();
      try { m.fit(Xr); emit("scaler.transform0", m.transform(probe)[0]);
      } finally { m.close(); } }

    { const w = Xr.map((_, i) => (i % 3 === 0 ? 3 : 1));
      let m = new LinearRegression();
      try { m.fit(Xr, yr); emit("linreg.sw.none", m.predict(probe)); } finally { m.close(); }
      m = new LinearRegression();
      try { m.fit(Xr, yr, { sampleWeight: Xr.map(() => 1) });
            emit("linreg.sw.ones", m.predict(probe)); } finally { m.close(); }
      m = new LinearRegression();
      try { m.fit(Xr, yr, { sampleWeight: w });
            emit("linreg.sw.weighted", m.predict(probe)); } finally { m.close(); }
    }
}
