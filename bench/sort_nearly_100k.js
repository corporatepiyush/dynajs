// x6 perf control: nearly-sorted 100k (fixed points = many skips removed)
const N = 100000;
const a = new Array(N);
for (let i = 0; i < N; i++) a[i] = i;
for (let i = 0; i < 100; i++) { const x = (i * 997) % N; const t = a[x]; a[x] = a[(x + 1) % N]; a[(x + 1) % N] = t; }
const t0 = Date.now();
a.sort((x, y) => x - y);
const t1 = Date.now();
print("ms=" + (t1 - t0));
