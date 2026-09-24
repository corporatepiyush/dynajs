/* bench_numeric.js -- number <-> string conversion (src/dtoa.c).
 *
 * RUN THIS BEFORE AND AFTER ANY CHANGE TO dtoa.c.
 *
 * Every proposal against that file (Ryu for base 10, killing the
 * shortest-representation search loop, 64-bit limbs, MSD-first digit
 * generation, splitting js_dtoa into backends) targets a different path, and
 * the paths differ by more than an order of magnitude. Measuring "number
 * formatting" as one number hides all of it.
 *
 * The axes that actually separate the implementations:
 *
 *   - FORMAT: free (shortest round-trip) vs fixed vs exponential vs precision.
 *     Only `free` runs the trial-and-error search loop.
 *   - RADIX: base 10 (bignum division per digit) vs power-of-two (bit
 *     extraction, no division at all). A change to the base-10 path must leave
 *     the hex path alone, so both are rows.
 *   - DIGIT COUNT: 1, 15, 17 significant digits, and toFixed(100). The bignum
 *     work is superlinear in digits; a single mid-sized case cannot show that.
 *   - PARSE vs FORMAT, separately: they are different code with different
 *     fast-path cutoffs.
 *
 * Output lines beginning with `#N` are machine-readable:
 *   #N <case> <ops> <ms> <Mops/s>
 *
 * Usage: dynajs tests/bench_numeric.js [scale]
 */
const SCALE = parseFloat(scriptArgs[1] || "1");

function row(name, reps, fn) {
    reps = Math.max(1, Math.round(reps * SCALE));
    fn(Math.max(1, reps / 20 | 0));                 /* warm */
    const t0 = performance.now();
    const sink = fn(reps);
    const ms = performance.now() - t0;
    print("#N " + name + " " + reps + " " + ms.toFixed(3) + " " +
          (reps / (ms / 1000) / 1e6).toFixed(2) + (sink === undefined ? "" : ""));
}

/* ---- FORMAT: free / shortest round-trip (the search loop) --------------- */
const easy  = 1.0, mid = 1234.5678, hard = 1.2345678901234567e-308;
const ints  = [0, 1, 42, 1e15, 2 ** 53 - 1];
row("fmt_free_easy",      2000000, n => { let s; for (let i = 0; i < n; i++) s = String(easy); return s; });
row("fmt_free_mid",        500000, n => { let s; for (let i = 0; i < n; i++) s = String(mid); return s; });
row("fmt_free_hard_subnormal", 200000, n => { let s; for (let i = 0; i < n; i++) s = String(hard); return s; });
row("fmt_free_smallint",  2000000, n => { let s; for (let i = 0; i < n; i++) s = String(ints[i & 3]); return s; });
row("fmt_free_random",     300000, n => { let s; for (let i = 0; i < n; i++) s = String(i * 1.0000001); return s; });

/* ---- FORMAT: fixed / exponential / precision (bignum digit generation) --- */
row("fmt_toFixed_2",       500000, n => { let s; for (let i = 0; i < n; i++) s = mid.toFixed(2); return s; });
row("fmt_toFixed_20",      200000, n => { let s; for (let i = 0; i < n; i++) s = mid.toFixed(20); return s; });
row("fmt_toFixed_100",      20000, n => { let s; for (let i = 0; i < n; i++) s = mid.toFixed(100); return s; });
row("fmt_toExponential_17",200000, n => { let s; for (let i = 0; i < n; i++) s = mid.toExponential(17); return s; });
row("fmt_toPrecision_21",  200000, n => { let s; for (let i = 0; i < n; i++) s = mid.toPrecision(21); return s; });

/* ---- FORMAT: radix (power-of-two bypasses division entirely) ------------- */
row("fmt_radix16",         500000, n => { let s; for (let i = 0; i < n; i++) s = (12345678).toString(16); return s; });
row("fmt_radix2",          300000, n => { let s; for (let i = 0; i < n; i++) s = (12345678).toString(2); return s; });
row("fmt_radix36",         300000, n => { let s; for (let i = 0; i < n; i++) s = (12345678).toString(36); return s; });
row("fmt_radix10_big",     200000, n => { let s; for (let i = 0; i < n; i++) s = (1.7976931348623157e308).toString(10); return s; });

/* ---- PARSE: fast path, its cutoff, and the slow path -------------------- */
const S15 = "100000000000000", S16 = "1000000000000000", S17 = "10000000000000000";
const F15 = "1.23456789012345", F16 = "1.234567890123456", F17 = "1.2345678901234567";
row("parse_int_15dig",    2000000, n => { let x = 0; for (let i = 0; i < n; i++) x += Number(S15); return x; });
row("parse_int_16dig",    2000000, n => { let x = 0; for (let i = 0; i < n; i++) x += Number(S16); return x; });
row("parse_int_17dig",    2000000, n => { let x = 0; for (let i = 0; i < n; i++) x += Number(S17); return x; });
row("parse_frac_15sig",   2000000, n => { let x = 0; for (let i = 0; i < n; i++) x += Number(F15); return x; });
row("parse_frac_17sig",   1000000, n => { let x = 0; for (let i = 0; i < n; i++) x += Number(F17); return x; });
row("parse_subnormal",     300000, n => { let x = 0; for (let i = 0; i < n; i++) x += Number("1.2345678901234567e-308"); return x; });
row("parse_hex",          1000000, n => { let x = 0; for (let i = 0; i < n; i++) x += Number("0xdeadbeefcafe"); return x; });
row("parse_exp",          1000000, n => { let x = 0; for (let i = 0; i < n; i++) x += Number("1.5e300"); return x; });

/* ---- Round trip: the property any replacement must preserve ------------- */
row("roundtrip_random",    200000, n => {
    let bad = 0;
    for (let i = 0; i < n; i++) { const v = i * 1.0000001e-3; if (Number(String(v)) !== v) bad++; }
    return bad;
});

print("#N DONE");
