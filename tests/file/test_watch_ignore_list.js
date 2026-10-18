// flags: --std
// Watcher's ignore list, found by the signature-driven input generator
// (tests/lib/sigfuzz.js): every entry must be a string, so a sparse list of
// length 2^32-1 fails at index 0 instead of being walked, and an exception
// from debounceMs's coercion propagates.
import { Watcher, Path } from "dyna:file";
import * as std from "std";
let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
async function aerr(f) { try { await f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const huge = () => { const a = []; a.length = 2 ** 32 - 1; return a; };
function fast(label, f, re) {
    const t0 = performance.now();
    const m = err(f);
    const dt = performance.now() - t0;
    ok(re.test(m), label + " (" + m + ")");
    ok(dt < 500, label + ": refused before any work (" + dt.toFixed(1) + " ms)");
}
const dir = new Path((std.getenv("TMPDIR") || "/tmp").replace(/\/+$/, ""));
fast("a sparse ignore list", () => new Watcher(dir, { ignore: huge() }), /^TypeError: Watcher: ignore\[0\] must be a string$/);
eq(err(() => new Watcher(dir, { ignore: ["*.tmp", 3] })), "TypeError: Watcher: ignore[1] must be a string", "a number entry is refused by index");
eq(err(() => new Watcher(dir, { debounceMs: { valueOf() { throw new RangeError("valueOf threw"); } } })), "RangeError: valueOf threw", "debounceMs's coercion error propagates");
{
    const w = new Watcher(dir, { ignore: ["*.tmp", "node_modules"] });
    ok(w instanceof Watcher, "a string ignore list is accepted (control)");
    w.close();
}

print("test_watch_ignore_list" + ": " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
