/* Call-frame cost: isolates the per-call prologue in JS_CallInternal.
 *
 * MEASURED NEGATIVE -- do not re-chase. The frame-slot fill loops
 * (interpreter.inc.c) reload b->var_count every iteration because the compiler
 * cannot prove the stores do not alias JSFunctionBytecode. Hoisting the bound
 * into a local + a hand 2-wide unroll DOES fix that: the slope below drops
 * 0.64 -> 0.46 ns/local and `call 32 locals` falls 28.4 -> 24.1 ms (-15%).
 * It buys NOTHING on real code: JetStream (richards/deltablue/raytrace/crypto/
 * navier-stokes/splay) moved -0.66%..+1.57%, i.e. noise in both directions,
 * because real functions declare a handful of locals, not 32. The 0-local call
 * -- by far the most common -- got 0.24 ns SLOWER from the extra pointer
 * variables. Reverted 2026-07-26. Keep this file as the proof and as the probe
 * to re-run if the call prologue is ever restructured for other reasons.
 *
 * The frame-init fills (var_buf[i]=JS_UNDEFINED, var_refs[i]=NULL) are the only
 * part of the prologue that scales with the callee's local count, so calling
 * functions that differ ONLY in how many locals they declare -- with bodies that
 * emit no code for them -- measures the fill directly. `var x,y,z;` with no
 * initialiser hoists a slot and emits nothing, which is what makes the isolation
 * exact; `var x=1` would add a push+put_loc pair per local and swamp the signal.
 *
 * Reported ns/local is the slope between the 32-local and 8-local points, so the
 * fixed call ceremony (~9 ns) cancels out.
 */
function bench(f) {
    for (let i = 0; i < 3; i++) f();
    let best = Infinity;
    for (let r = 0; r < 7; r++) {
        const t0 = performance.now(); f(); const t1 = performance.now();
        if (t1 - t0 < best) best = t1 - t0;
    }
    return best;
}

const N = 3000000;

function f0() { return 1; }
function fv8()  { var a1,a2,a3,a4,a5,a6,a7,a8; return 1; }
function fv32() { var a1,a2,a3,a4,a5,a6,a7,a8,b1,b2,b3,b4,b5,b6,b7,b8,
                      c1,c2,c3,c4,c5,c6,c7,c8,d1,d2,d3,d4,d5,d6,d7,d8; return 1; }
function fv64() { var a1,a2,a3,a4,a5,a6,a7,a8,b1,b2,b3,b4,b5,b6,b7,b8,
                      c1,c2,c3,c4,c5,c6,c7,c8,d1,d2,d3,d4,d5,d6,d7,d8,
                      e1,e2,e3,e4,e5,e6,e7,e8,g1,g2,g3,g4,g5,g6,g7,g8,
                      h1,h2,h3,h4,h5,h6,h7,h8,i1,i2,i3,i4,i5,i6,i7,i8; return 1; }

/* closures force var_ref_count > 0 so the second fill loop is exercised too */
function fclo() { var a1,a2,a3,a4,a5,a6,a7,a8; return function () { return a1+a8; }; }

const base = bench(() => { let s = 0; for (let i = 0; i < N; i++) s += 1; return s; });
const c0   = bench(() => { let s = 0; for (let i = 0; i < N; i++) s += f0();   return s; });
const c8   = bench(() => { let s = 0; for (let i = 0; i < N; i++) s += fv8();  return s; });
const c32  = bench(() => { let s = 0; for (let i = 0; i < N; i++) s += fv32(); return s; });
const c64  = bench(() => { let s = 0; for (let i = 0; i < N; i++) s += fv64(); return s; });
const cclo = bench(() => { let s = 0; for (let i = 0; i < N; i++) s += (fclo() ? 1 : 0); return s; });

const ns = (t) => (t - base) * 1e6 / N;

console.log("call  0 locals      " + ns(c0).toFixed(2)   + " ns");
console.log("call  8 locals      " + ns(c8).toFixed(2)   + " ns");
console.log("call 32 locals      " + ns(c32).toFixed(2)  + " ns");
console.log("call 64 locals      " + ns(c64).toFixed(2)  + " ns");
console.log("closure(8 captured) " + ns(cclo).toFixed(2) + " ns");
console.log("SLOPE ns/local      " + ((ns(c32) - ns(c8)) / 24).toFixed(3) +
            "   (64-8: " + ((ns(c64) - ns(c8)) / 56).toFixed(3) + ")");
