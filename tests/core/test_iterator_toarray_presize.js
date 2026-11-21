// covers: c:src/builtins/builtin-iterator.c
// Iterator.prototype.toArray presizes its result from a size hint when the source is a pristine %ArrayIterator% over a
// fast array or typed array, optionally under map/take/drop helpers (map keeps the count, take caps it, drop subtracts),
// then appends in place. The hint only sizes the backing store: results, lengths and observable steps are pinned by
// test_iterator_trace_oracle.js; this suite pins the memory side.
// - "exact": a mapped/dropped/plain toArray of n elements holds about n 16-byte slots (n = 1020 sits under a 16 KiB malloc
//   size class, so geometric growth shows as 1.25x or more);
// - "capped": take(k) over a large array holds about k slots, not the source length;
// - "unknown": sources without a hint (Set, generator, filter) still give the right values.
import { memoryUsage } from "dyna:sys";
import { sections } from "../lib/kit.js";
let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }
const S = sections("test_iterator_toarray_presize");
function perResult(make, reps) {
    const keep = [];
    make();
    const before = memoryUsage().mallocSize;
    for (let i = 0; i < reps; i++) keep.push(make());
    const after = memoryUsage().mallocSize;
    return { bytes: (after - before) / reps, last: keep[reps - 1] };
}
const n = 1020, src = Array.from({ length: n }, (_, i) => i), u8 = new Uint8Array(n).map((_, i) => i & 255);
S.section("exact", () => {
    for (const [name, make, len] of [
        ["values", () => src.values().toArray(), n],
        ["map", () => src.values().map((x) => x + 1).toArray(), n],
        ["drop", () => src.values().drop(5).toArray(), n - 5],
        ["typed map", () => u8.values().map((x) => x * 2).toArray(), n],
    ]) {
        const { bytes, last } = perResult(make, 64);
        ok(last.length === len, name + ": length " + last.length);
        ok(bytes < len * 16 * 1.1 + 256, name + ": " + bytes.toFixed(0) + " bytes per result for " + len + " elements (want about " + len * 16 + "; growth by doubling or 1.5x holds 1.25x or more here)");
    }
});
S.section("capped", () => {
    const big = Array.from({ length: 1 << 16 }, (_, i) => i);
    const { bytes, last } = perResult(() => big.values().take(4).toArray(), 64);
    ok(last.join() === "0,1,2,3", "take(4) values " + last.join());
    ok(bytes < 1024, "take(4) over 65536 elements holds " + bytes.toFixed(0) + " bytes per result (want under 1 KiB)");
    const d = perResult(() => big.values().drop(65530).toArray(), 64);
    ok(d.last.join() === "65530,65531,65532,65533,65534,65535", "drop tail values");
    ok(d.bytes < 1024, "drop(65530) holds " + d.bytes.toFixed(0) + " bytes per result (want under 1 KiB)");
});
S.section("unknown", () => {
    function* g() { yield 1; yield 2; }
    ok(new Set([1, 2, 3]).values().toArray().join() === "1,2,3", "Set source");
    ok(g().toArray().join() === "1,2", "generator source");
    ok(src.values().filter((x) => x % 2).toArray().length === n / 2, "filter source");
});
S.end();
print(fail ? "FAIL " + fail + " of " + (pass + fail) : "PASS " + pass);
