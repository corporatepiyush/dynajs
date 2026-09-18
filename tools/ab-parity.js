/* ab-parity.js -- prove two binaries in an A/B were built the SAME way.
 *
 * Run it on both and compare the lines:
 *     ./old/dynajs tools/ab-parity.js
 *     ./dynajs     tools/ab-parity.js
 *
 * WHY THIS EXISTS. A fresh checkout is not automatically a valid baseline.
 * Anything the ignore rules exclude is absent from it, and its absence changes
 * what gets built without changing what compiles: `third_party/` is gitignored
 * with zero tracked files, so a `git worktree` baseline links the SYSTEM libm
 * instead of the vendored openlibm. Both export the same symbols, so checking
 * that a symbol exists proves nothing -- the probe has to observe a
 * BEHAVIOURAL difference, and the last places of transcendentals are exactly
 * where two libm implementations disagree.
 *
 * If the LIBM lines differ, the baseline is invalid and every number measured
 * against it is void. If the MODULES lines differ, the two binaries are running
 * different programs, not the same one at different speeds.
 */
import * as std from "std";

function fnv(s) {
  let h = 2166136261 >>> 0;
  for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 16777619) >>> 0; }
  return ("00000000" + h.toString(16)).slice(-8);
}

/* Full 17-digit round-trip form, so a last-ULP difference changes the string. */
const parts = [];
const xs = [];
for (let i = 1; i <= 200; i++) xs.push(i * 0.37, -i * 1.7, i / 3, Math.sqrt(i), 1 / i);
for (const x of xs) {
  parts.push(Math.sin(x), Math.cos(x), Math.tan(x), Math.exp(x), Math.atan(x),
             Math.sinh(x), Math.tanh(x), Math.cbrt(x), Math.expm1(x));
  if (x > 0) parts.push(Math.log(x), Math.log1p(x), Math.log2(x), Math.log10(x),
                        Math.pow(x, 1.7), Math.asinh(x));
}
print("LIBM    " + fnv(parts.map((v) => (Number.isFinite(v) ? v.toPrecision(17) : String(v))).join(",")));

/* Which dyna:* modules this binary carries. A baseline missing one runs a
 * different program, not a slower one. The STATIC import decides module-vs-
 * script parsing, so each probe is its own dynamic import guarded per name. */
const MODS = ["serialize", "dataframe", "csv", "simd", "net", "file", "encoding",
              "time", "ml", "structures", "crypto", "compress", "mathx"];
const present = [];
let pending = MODS.length;
for (const m of MODS) {
  import("dyna:" + m).then(() => { present.push(m); done(); }, () => done());
}
function done() {
  if (--pending) return;
  present.sort();
  print("MODULES " + (present.length ? present.join(",") : "(none)"));
  print("ARGV0   " + (scriptArgs[0] || "?"));
  std.exit(0);
}
