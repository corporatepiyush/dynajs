import { LinearRegression, LogisticRegression, KMeans,
         GaussianNB, GaussianMixture } from "dyna:ml";

const REPS = 5;

function bench(name, setup, run) {
    const ctx = setup();
    run(ctx);
    let best = Infinity;
    for (let r = 0; r < REPS; r++) {
        const t0 = performance.now();
        run(ctx);
        const dt = performance.now() - t0;
        if (dt < best) best = dt;
    }
    print(name.padEnd(42) + best.toFixed(3).padStart(10) + " ms");
    return best;
}

function rng(seed) {
    let s = seed >>> 0;
    return () => {
        s = (s * 1664525 + 1013904223) >>> 0;
        return s / 4294967296;
    };
}

function makeRegression(rows, cols, seed) {
    const rnd = rng(seed);
    const X = new Float64Array(rows * cols);
    const y = new Float64Array(rows);
    const w = new Float64Array(cols);
    for (let j = 0; j < cols; j++) w[j] = rnd() * 2 - 1;
    for (let i = 0; i < rows; i++) {
        let acc = 0.5;
        for (let j = 0; j < cols; j++) {
            const v = rnd() * 2 - 1;
            X[i * cols + j] = v;
            acc += w[j] * v;
        }
        y[i] = acc + (rnd() - 0.5) * 0.01;
    }
    return { X, y, rows, cols };
}

function makeClassification(rows, cols, seed) {
    const d = makeRegression(rows, cols, seed);
    for (let i = 0; i < rows; i++) d.y[i] = d.y[i] > 0.5 ? 1 : 0;
    return d;
}

function makeClusters(rows, cols, k, seed) {
    const rnd = rng(seed);
    const X = new Float64Array(rows * cols);
    const centres = new Float64Array(k * cols);
    for (let c = 0; c < k * cols; c++) centres[c] = rnd() * 20 - 10;
    for (let i = 0; i < rows; i++) {
        const c = i % k;
        for (let j = 0; j < cols; j++)
            X[i * cols + j] = centres[c * cols + j] + (rnd() - 0.5);
    }
    return { X, rows, cols, k };
}

print("dyna:ml — best of " + REPS + "\n");

for (const [rows, cols] of [[100000, 4], [20000, 32], [4000, 128]]) {
    const d = makeRegression(rows, cols, 12345);
    bench(`linreg.fit      rows=${rows} cols=${cols}`,
        () => new LinearRegression(),
        (m) => m.fit(d.X, d.y, d.rows, d.cols));
}
for (const [rows, cols] of [[200000, 4], [200000, 32], [50000, 128]]) {
    const d = makeRegression(rows, cols, 999);
    const m = new LinearRegression().fit(d.X, d.y, d.rows, d.cols);
    bench(`linreg.predict  rows=${rows} cols=${cols}`,
        () => m,
        (mm) => mm.predict(d.X, d.rows, d.cols));
}

for (const [rows, cols] of [[2000, 4], [2000, 32], [500, 128]]) {
    const d = makeClassification(rows, cols, 777);
    bench(`logreg.fit      rows=${rows} cols=${cols}`,
        () => new LogisticRegression(),
        (m) => m.fit(d.X, d.y, d.rows, d.cols));
}
{
    const d = makeClassification(100000, 32, 31337);
    const small = makeClassification(500, 32, 31337);
    const m = new LogisticRegression().fit(small.X, small.y, small.rows, small.cols);
    bench(`logreg.predictProba rows=100000 cols=32`,
        () => m,
        (mm) => mm.predictProba(d.X, 100000, 32));
}

for (const [rows, cols, k] of [[20000, 4, 8], [20000, 32, 8], [5000, 128, 8]]) {
    const d = makeClusters(rows, cols, k, 4242);
    bench(`kmeans.fit      rows=${rows} cols=${cols} k=${k}`,
        () => new KMeans(k, 7),
        (m) => m.fit(d.X, d.rows, d.cols));
}
{
    const d = makeClusters(50000, 32, 8, 5150);
    const m = new KMeans(8, 7).fit(d.X, d.rows, d.cols);
    bench(`kmeans.predict  rows=50000 cols=32 k=8`,
        () => m,
        (mm) => mm.predict(d.X, d.rows, d.cols));
}

for (const [rows, cols] of [[50000, 8], [50000, 32], [20000, 128]]) {
    const d = makeClassification(rows, cols, 1234);
    for (let i = 0; i < rows; i++) d.y[i] = i % 3;
    const m = new GaussianNB().fit(d.X, d.y, rows, cols);
    bench(`gaussianNB.predict rows=${rows} cols=${cols} k=3`,
        () => m, (mm) => mm.predict(d.X, rows, cols));
    bench(`gaussianNB.proba   rows=${rows} cols=${cols} k=3`,
        () => m, (mm) => mm.predictProba(d.X, rows, cols));
}

for (const [rows, cols, k] of [[10000, 8, 4], [10000, 32, 4], [4000, 128, 4]]) {
    const d = makeClusters(rows, cols, k, 4242);
    bench(`gmm.fit         rows=${rows} cols=${cols} k=${k}`,
        () => new GaussianMixture(k, { seed: 7, maxIter: 25, tol: 0 }),
        (m) => m.fit(d.X, d.rows, d.cols));
}
{
    const d = makeClusters(50000, 32, 4, 99);
    const m = new GaussianMixture(4, { seed: 7 }).fit(d.X, d.rows, d.cols);
    bench(`gmm.predictProba rows=50000 cols=32 k=4`,
        () => m, (mm) => mm.predictProba(d.X, d.rows, d.cols));
}
