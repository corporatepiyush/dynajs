// perf gate for x6: dense 100k array sort must not regress >5%
const N = 100000;
let seed = 12345;
function rnd() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed; }
const a = new Array(N);
for (let i = 0; i < N; i++) a[i] = rnd();
const t0 = Date.now();
a.sort((x, y) => x - y);
const t1 = Date.now();
let ok = 0;
for (let i = 1; i < N; i++) if (a[i - 1] <= a[i]) ok++;
print("sorted=" + (ok === N - 1) + " ms=" + (t1 - t0));
