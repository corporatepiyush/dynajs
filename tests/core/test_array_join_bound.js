// Array.prototype.join refuses a result longer than any string can be BEFORE
// walking the array (found by tests/lib/sigfuzz.js): length 2^32-1 with a
// non-empty separator is at least 2^32-2 separator characters, past
// JS_STRING_LEN_MAX, so the RangeError the walk would end in comes at once.
// (2^29 slots with "ab" is 2^30-2 characters, which fits, and is built.)
// An empty separator has no such bound and is not covered here: its result
// may legitimately be short, so the walk is the specification's.
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
for (const sep of [",", "ab", "\u{1F600}"]) fast("a 2^32-1 sparse array joined with " + JSON.stringify(sep), () => huge().join(sep), /^RangeError: invalid string length$/);
{
    const a = [];
    a.length = 2 ** 29 + 1;
    fast("2^29+1 slots with a 2-char separator is 2^30 characters, one past the maximum", () => a.join("ab"), /^RangeError: invalid string length$/);
}
{
    const a = [];
    a.length = 1000;
    eq(a.join(",").length, 999, "a 1000-slot sparse array joins to 999 commas (control)");
}
eq([1, [2, 3], null, undefined, "x"].join("-"), "1-2,3---x", "ordinary join (control)");
{
    const a = { length: 3, 0: "a", 1: "b", 2: "c" };
    eq(Array.prototype.join.call(a, "+"), "a+b+c", "array-likes still join (control)");
}

print("test_array_join_bound" + ": " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
