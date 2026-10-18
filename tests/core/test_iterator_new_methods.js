// Iterator.prototype.chunks/windows (TC39 proposal-iterator-chunking, Stage 3; the upstream test262
// chunks/ and windows/ directories pass), Iterator.range (TC39 proposal-iterator.range, Stage 2; checked
// against a transcription of CreateNumericRangeIterator below), and the dynajs extensions withIndex,
// enumerate, flat, cycle, stepBy, tap, partition, toSet, toMap, join, minBy, maxBy.
let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }
const show = (v) => JSON.stringify(v, (k, x) => (typeof x === "bigint" ? x + "n" : x));
function eq(a, b, m) { ok(show(a) === show(b), m + " (got " + show(a) + ", want " + show(b) + ")"); }
function throws(ctor, fn, m) { let e = null; try { fn(); } catch (x) { e = x; } ok(e instanceof ctor, m + " throws " + ctor.name + " (got " + e + ")"); }
function counted(n) {
    const log = { next: 0, ret: 0 };
    let i = 0;
    const it = Object.setPrototypeOf({ next() { log.next++; return i < n ? { value: i++, done: false } : { value: undefined, done: true }; }, return() { log.ret++; return {}; } }, Iterator.prototype);
    return { it, log };
}

eq([0, 1, 2, 3, 4].values().chunks(2).toArray(), [[0, 1], [2, 3], [4]], "chunks keeps the short last chunk");
eq([0, 1, 2, 3].values().chunks(2).toArray(), [[0, 1], [2, 3]], "chunks evenly divisible");
eq([].values().chunks(3).toArray(), [], "chunks of nothing");
eq([1, 2].values().chunks(5).toArray(), [[1, 2]], "chunks larger than the source");
eq([0, 1, 2, 3].values().windows(2).toArray(), [[0, 1], [1, 2], [2, 3]], "windows overlap");
eq([0, 1].values().windows(3).toArray(), [], "windows only-full yields nothing when short");
eq([0, 1].values().windows(3, "allow-partial").toArray(), [[0, 1]], "windows allow-partial yields the short window");
eq([0, 1, 2].values().windows(3, "allow-partial").toArray(), [[0, 1, 2]], "allow-partial with an exact fit");
eq([0, 1, 2, 3].values().windows(2, "only-full").toArray(), [[0, 1], [1, 2], [2, 3]], "explicit only-full");
{
    const w = [1, 2, 3].values().windows(2).toArray();
    w[0][0] = 99;
    eq(w, [[99, 2], [2, 3]], "each window is a fresh array");
}
for (const [m, args] of [["chunks", [0]], ["chunks", [-1]], ["chunks", [2 ** 32]], ["windows", [0]], ["windows", [2 ** 32]]]) {
    const { it, log } = counted(3);
    throws(RangeError, () => it[m](...args), m + "(" + args + ")");
    ok(log.ret === 1, m + "(" + args + ") closes the source");
}
for (const [m, args] of [["chunks", ["2"]], ["chunks", [1.5]], ["chunks", [NaN]], ["chunks", [Infinity]], ["windows", [2, "partial"]], ["windows", [2, null]], ["windows", [2, 1]]]) {
    const { it, log } = counted(3);
    throws(TypeError, () => it[m](...args), m + "(" + args.map(String) + ")");
    ok(log.ret === 1, m + "(" + args.map(String) + ") closes the source");
}
{
    const { it, log } = counted(5);
    const c = it.chunks(2);
    eq(c.next().value, [0, 1], "chunks first");
    c.return();
    ok(log.ret === 1, "chunks return() closes a live source");
    const s = counted(3);
    const c2 = s.it.chunks(2);
    c2.next();
    eq(c2.next().value, [2], "partial chunk after exhaustion");
    c2.return();
    ok(s.log.ret === 0, "return() after the source finished does not close it");
    ok(s.log.next === 4, "the source is not pulled after it finished (" + s.log.next + ")");
    const t = counted(3);
    const c3 = t.it.chunks(2);
    c3.next();
    c3.next();
    eq(c3.next(), { done: true }, "chunks is done after the partial chunk");
    ok(t.log.next === 4, "chunks does not pull an exhausted source again (" + t.log.next + ")");
}
eq([1, 2, 3, 4].values().aperture(2).toArray(), [[1, 2], [2, 3], [3, 4]], "aperture is only-full windows");
eq([1, 2, 3].values().splitEvery(2).toArray(), [[1, 2], [3]], "splitEvery is chunks");
eq([1, 2].values().aperture(5).toArray(), [], "aperture larger than the source");
throws(RangeError, () => [1].values().aperture(0), "aperture(0)");
eq([1, 2, 3].values().splitEvery("2").toArray(), [[1, 2], [3]], "splitEvery coerces its size");
{
    const big = Array.from({ length: 1000 }, (_, i) => i);
    const w = big.values().windows(100).toArray();
    ok(w.length === 901 && w[900][0] === 900 && w[900][99] === 999 && w[450][50] === 500, "windows(100) over 1000 wraps the ring correctly");
    const c = big.values().chunks(64).toArray();
    ok(c.length === 16 && c[15].length === 40 && c[15][39] === 999, "chunks(64) over 1000");
}

eq(["a", "b"].values().withIndex().toArray(), [[0, "a"], [1, "b"]], "withIndex");
ok(Iterator.prototype.enumerate === Iterator.prototype.withIndex, "enumerate is withIndex");
eq([[1, 2], [3], []].values().flat().toArray(), [1, 2, 3], "flat");
eq([new Set([1, 2]), "xy".split("")].values().flat().toArray(), [1, 2, "x", "y"], "flat over iterables");
throws(TypeError, () => [1].values().flat().toArray(), "flat of a primitive");
eq([1, 2].values().cycle().take(5).toArray(), [1, 2, 1, 2, 1], "cycle repeats");
eq([].values().cycle().take(3).toArray(), [], "cycle of nothing ends");
{
    const s = counted(2);
    const c = s.it.cycle();
    eq(c.take(5).toArray(), [0, 1, 0, 1, 0], "cycle over a custom source");
    ok(s.log.next === 3 && s.log.ret === 0, "cycle reads the source once and does not close it after exhaustion (next " + s.log.next + ", return " + s.log.ret + ")");
    const t = counted(5);
    const c2 = t.it.cycle();
    c2.next();
    c2.return();
    ok(t.log.ret === 1, "cycle return() before exhaustion closes the source");
}
eq([0, 1, 2, 3, 4, 5, 6].values().stepBy(3).toArray(), [0, 3, 6], "stepBy");
eq([0, 1, 2].values().stepBy(1).toArray(), [0, 1, 2], "stepBy(1)");
eq([0, 1, 2].values().stepBy(Infinity).toArray(), [0], "stepBy(Infinity)");
throws(RangeError, () => [1].values().stepBy(0), "stepBy(0)");
throws(RangeError, () => [1].values().stepBy(NaN), "stepBy(NaN)");
{
    const seen = [];
    eq([5, 6].values().tap((v, i) => seen.push([v, i])).map((x) => x * 2).toArray(), [10, 12], "tap passes values through");
    eq(seen, [[5, 0], [6, 1]], "tap sees value and index");
    throws(TypeError, () => [1].values().tap(1), "tap(non-function)");
    eq([1, 2].values().tap(() => ({ owned: [1, 2, 3] })).toArray(), [1, 2], "tap discards an object result");
    eq([1, 2].values().partition(() => ({})), [[1, 2], []], "partition treats an object result as truthy");
}
eq([1, 2, 3, 4, 5].values().partition((x, i) => x % 2 === 1), [[1, 3, 5], [2, 4]], "partition");
eq([].values().partition(() => true), [[], []], "partition of nothing");
{
    const s = [1, 2, 2, 3].values().toSet();
    ok(s instanceof Set && s.size === 3 && s.has(2), "toSet");
    const m = [["a", 1], ["b", 2], ["a", 3]].values().toMap();
    ok(m instanceof Map && m.size === 2 && m.get("a") === 3 && m.get("b") === 2, "toMap last write wins");
    const { it, log } = counted(2);
    throws(TypeError, () => it.toMap(), "toMap of non-objects");
    ok(log.ret === 1, "toMap closes the source on a bad entry");
}
eq([1, null, undefined, "x", 2].values().join(), "1,,,x,2", "join default separator");
eq([1, 2].values().join(" - "), "1 - 2", "join separator");
eq([].values().join("x"), "", "join of nothing");
eq([1, 2].values().join(0), "102", "join coerces the separator");
eq(["bb", "a", "ccc"].values().minBy((s) => s.length), "a", "minBy");
eq(["bb", "a", "ccc"].values().maxBy((s) => s.length), "ccc", "maxBy");
eq(["pear", "apple", "fig"].values().minBy((s) => s), "apple", "minBy compares strings as strings");
eq([{ k: 2, v: "a" }, { k: 1, v: "b" }, { k: 1, v: "c" }].values().minBy((o) => o.k).v, "b", "minBy keeps the first minimum");
eq([{ k: 2, v: "a" }, { k: 2, v: "b" }].values().maxBy((o) => o.k).v, "a", "maxBy keeps the first maximum");
eq([3n, 10n, 2n].values().maxBy((x) => x), 10n, "maxBy over bigints");
eq([].values().minBy((x) => x), undefined, "minBy of nothing");
eq([2, 3].values().min((x) => -x), 3, "min(fn) is numeric (unchanged)");
for (const m of ["partition", "minBy", "maxBy"]) {
    const { it, log } = counted(2);
    throws(TypeError, () => it[m](1), m + "(non-function)");
    ok(log.ret === 1, m + "(non-function) closes the source");
}
{
    const { it, log } = counted(3);
    let e = null;
    try { it.minBy(() => { throw new Error("k"); }); } catch (x) { e = x; }
    ok(e && e.message === "k" && log.ret === 1, "a throwing key closes the source");
}

function refRange(start, end, optionOrStep) {
    let type;
    if (typeof start === "number") type = "number";
    else if (typeof start === "bigint") type = "bigint";
    else throw new TypeError("start");
    if (Number.isNaN(start) || Number.isNaN(end)) throw new RangeError("nan");
    let zero, one;
    if (type === "number") { if (typeof end !== "number") throw new TypeError("end"); zero = 0; one = 1; }
    else { if (typeof end !== "bigint" && end !== Infinity && end !== -Infinity) throw new TypeError("end"); zero = 0n; one = 1n; }
    if (start === Infinity || start === -Infinity) throw new RangeError("start");
    let inclusiveEnd = false, step;
    if (optionOrStep === undefined || optionOrStep === null) step = undefined;
    else if (typeof optionOrStep === "object") { step = optionOrStep.step; inclusiveEnd = !!optionOrStep.inclusive; }
    else if (typeof optionOrStep === type) step = optionOrStep;
    else throw new TypeError("optionOrStep");
    if (step === undefined || step === null) {
        if (end === Infinity) step = one;
        else if (end === -Infinity) step = -one;
        else if (end > start) step = one;
        else step = -one;
    }
    if (Number.isNaN(step)) throw new RangeError("step nan");
    if (typeof step !== type) throw new TypeError("step type");
    if (step === Infinity || step === -Infinity) throw new RangeError("step inf");
    if (step == zero && start !== end) throw new RangeError("step zero");
    const out = [];
    const ifIncrease = end === Infinity ? true : end === -Infinity ? false : end > start;
    const ifStepIncrease = step > zero;
    if (ifIncrease !== ifStepIncrease) return out;
    let hitsEnd = false, currentCount = zero;
    while (!hitsEnd && out.length < 60) {
        const v = start + step * currentCount;
        if (v === end) hitsEnd = true;
        currentCount = currentCount + one;
        if (ifIncrease) { if (inclusiveEnd ? v > end : v >= end) break; }
        else { if (inclusiveEnd ? end > v : end >= v) break; }
        out.push(v);
    }
    return out;
}
function runRange(...a) {
    try { return { v: Iterator.range(...a).take(60).toArray() }; } catch (e) { return { e: e.constructor.name }; }
}
function runRef(...a) {
    try { return { v: refRange(...a) }; } catch (e) { return { e: e.constructor.name }; }
}
{
    const nums = [0, 1, -1, 3, 0.5, -2.5, 10, NaN, Infinity, -Infinity];
    const opts = [undefined, null, 1, -1, 0, 0.3, 2, NaN, Infinity, { step: 3 }, { inclusive: true }, { step: -0.5, inclusive: true }, { step: "1" }, "x", 2n];
    let cases = 0, agree = 0;
    for (const s of nums) for (const e of nums) for (const o of opts) {
        cases++;
        const a = runRange(s, e, o), b = runRef(s, e, o);
        if (show(a) === show(b)) agree++;
        else if (fail < 20) ok(false, `range(${s}, ${e}, ${show(o)}) got ${show(a)} want ${show(b)}`);
    }
    ok(agree === cases, `number ranges agree with the spec transcription (${agree}/${cases})`);
    const bigs = [0n, 1n, -1n, 5n, -3n, Infinity, -Infinity, 2, NaN];
    const bopts = [undefined, 1n, -1n, 0n, 2n, { step: 2n, inclusive: true }, { step: 1 }, 1];
    cases = 0; agree = 0;
    for (const s of [0n, 5n, -3n, 1]) for (const e of bigs) for (const o of bopts) {
        cases++;
        const a = runRange(s, e, o), b = runRef(s, e, o);
        if (show(a) === show(b)) agree++;
        else if (fail < 40) ok(false, `range(${show(s)}, ${show(e)}, ${show(o)}) got ${show(a)} want ${show(b)}`);
    }
    ok(agree === cases, `bigint ranges agree with the spec transcription (${agree}/${cases})`);
}
eq(Iterator.range(0, 5).toArray(), [0, 1, 2, 3, 4], "range(0, 5)");
eq(Iterator.range(0, 1, 0.1).toArray().length, 10, "0.1 steps are computed from the index, not accumulated");
eq(Iterator.range(0, 1, 0.1).toArray()[3], 0.30000000000000004, "the fourth value is 3 * 0.1");
eq(Iterator.range(0n, 3n).toArray(), [0n, 1n, 2n], "bigint range");
eq(Iterator.range(5, 5, { inclusive: true }).toArray(), [5], "start == end inclusive");
eq(Iterator.range(5, 5).toArray(), [], "start == end exclusive");
eq(Iterator.range(0, Infinity).take(3).toArray(), [0, 1, 2], "infinite range");
eq(Iterator.range(0n, -Infinity).take(3).toArray(), [0n, -1n, -2n], "infinite bigint range");
{
    const r = Iterator.range(0, 3);
    ok(Object.prototype.toString.call(r) === "[object NumericRangeIterator]", "range toStringTag");
    ok(Object.getPrototypeOf(Object.getPrototypeOf(r)) === Iterator.prototype, "range inherits Iterator.prototype");
    ok(typeof r.map === "function" && r.map((x) => x * 2).toArray().join() === "0,2,4", "range has the helpers");
    ok(Iterator.range.length === 3, "Iterator.range.length");
    let e = null;
    try { Object.getPrototypeOf(r).next.call({}); } catch (x) { e = x; }
    ok(e instanceof TypeError, "next on a foreign object throws");
    const gets = [];
    Iterator.range(0, 3, { get step() { gets.push("step"); return 1; }, get inclusive() { gets.push("inclusive"); return false; } });
    eq(gets, ["step", "inclusive"], "options are read in order: step, inclusive");
}

print(`iterator new methods: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failures");
