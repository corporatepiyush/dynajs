/* Object construction + shape machinery.
 *
 * The profiler puts b_objects self-time in add_shape_property /
 * add_property / JS_NewObjectFromShape / __js_malloc -- construction, not reads.
 * This file separates the two so a change can be attributed, and pins the two
 * facts that were measured before any of this work started:
 *
 *   * the property PROBE does not degrade with object width (a 1024-property
 *     object reads as fast as a 3-property one), so the shape hash map is not
 *     the bottleneck and does not need replacing;
 *   * construction via a constructor with a fixed field set is ~2.5x cheaper
 *     per property than the equivalent object literal, because the constructor
 *     path is pre-sized (CONFIG_PRESIZE_CTOR) and the literal path pays a shape
 *     transition per field. That gap is the object-literal pre-sizing lever.
 *
 * Also guards find_hashed_shape_prop: the transition search must stay flat in
 * property count.
 */
function bench(name, f) {
    for (let i = 0; i < 3; i++) f();
    let best = Infinity;
    for (let r = 0; r < 5; r++) {
        const t0 = performance.now(); f(); const t1 = performance.now();
        if (t1 - t0 < best) best = t1 - t0;
    }
    console.log(name.padEnd(44) + best.toFixed(3) + " ms");
    return best;
}

const N = 300000;
const R = 3000000;

console.log("--- construction: object literal (shape transition per field) ---");
const L = {};
L[2]  = bench("literal  2 fields x300k", () => { let s = 0; for (let i = 0; i < N; i++) { const o = {a:i,b:i}; s += o.b; } return s; });
L[4]  = bench("literal  4 fields x300k", () => { let s = 0; for (let i = 0; i < N; i++) { const o = {a:i,b:i,c:i,d:i}; s += o.d; } return s; });
L[8]  = bench("literal  8 fields x300k", () => { let s = 0; for (let i = 0; i < N; i++) { const o = {a:i,b:i,c:i,d:i,e:i,f:i,g:i,h:i}; s += o.h; } return s; });
L[16] = bench("literal 16 fields x300k", () => { let s = 0; for (let i = 0; i < N; i++) { const o = {a:i,b:i,c:i,d:i,e:i,f:i,g:i,h:i,i2:i,j:i,k:i,l:i,m:i,n:i,o2:i,p:i}; s += o.p; } return s; });

console.log("--- construction: class ctor (pre-sized) ---");
class C2  { constructor(i){ this.a=i; this.b=i; } }
class C4  { constructor(i){ this.a=i; this.b=i; this.c=i; this.d=i; } }
class C8  { constructor(i){ this.a=i; this.b=i; this.c=i; this.d=i; this.e=i; this.f=i; this.g=i; this.h=i; } }
class C16 { constructor(i){ this.a=i; this.b=i; this.c=i; this.d=i; this.e=i; this.f=i; this.g=i; this.h=i;
                            this.i2=i; this.j=i; this.k=i; this.l=i; this.m=i; this.n=i; this.o2=i; this.p=i; } }
const K = {};
K[2]  = bench("ctor  2 fields x300k", () => { let s = 0; for (let i = 0; i < N; i++) s += new C2(i).b;  return s; });
K[4]  = bench("ctor  4 fields x300k", () => { let s = 0; for (let i = 0; i < N; i++) s += new C4(i).d;  return s; });
K[8]  = bench("ctor  8 fields x300k", () => { let s = 0; for (let i = 0; i < N; i++) s += new C8(i).h;  return s; });
K[16] = bench("ctor 16 fields x300k", () => { let s = 0; for (let i = 0; i < N; i++) s += new C16(i).p; return s; });

console.log("\nns per property stored:");
for (const k of [2, 4, 8, 16])
    console.log("  " + String(k).padStart(2) + " fields   literal " +
        (L[k] * 1e6 / (N * k)).toFixed(1).padStart(5) + "   ctor " +
        (K[k] * 1e6 / (N * k)).toFixed(1).padStart(5) + "   ratio " +
        (L[k] / K[k]).toFixed(2) + "x");

console.log("\n--- read probe: must stay FLAT in object width ---");
/* One live wide object at a time: holding several alive at once perturbs this
   measurement through allocator/cache effects, not through probe depth. */
const base = bench("empty loop x3M", () => { let s = 0; for (let i = 0; i < R; i++) s += 1; return s; });
for (const k of [4, 64, 1024]) {
    const o = {};
    for (let i = 0; i < k; i++) o["p" + i] = i;
    const first = bench("read p0     from " + k + "-prop obj", () => { let s = 0; for (let i = 0; i < R; i++) s += o.p0; return s; });
    console.log("     -> " + ((first - base) * 1e6 / R).toFixed(2) + " ns/read");
}

console.log("\n--- dictionary-mode object: insert must stay FLAT in key count ---");
const keys = [];
for (let i = 0; i < 200000; i++) keys.push("key_" + i);
for (const n of [2000, 32000, 128000]) {
    const o = {};
    const t0 = performance.now();
    for (let i = 0; i < n; i++) o[keys[i]] = i;
    const t1 = performance.now();
    console.log("  " + String(n).padStart(6) + " keys   " +
        ((t1 - t0) * 1e6 / n).toFixed(0).padStart(4) + " ns/insert");
}
