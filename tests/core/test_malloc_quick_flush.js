// flags: --std
// covers: c:src/mm/js_malloc.inc.c
// The small-block allocator keeps up to JS_MALLOC_QUICK_MAX freed blocks per
// size class on a LIFO stack so the next allocation of that class does not
// touch its arena header. Those blocks still count as used in their arenas, so
// every GC hands them back (js_malloc_flush_quick): after std.gc() the arena
// set, and therefore memoryUsage().mallocCount, must be what the arena-only
// allocator leaves. The leak suites that compare mallocCount across std.gc()
// depend on that. Without the flush this churn retains 48 arenas; with it 13
// (the same as the allocator before the stacks existed).
import * as std from "std";
import { memoryUsage } from "dyna:sys";
import { sections } from "../lib/kit.js";

const S = sections("test_malloc_quick_flush");
S.section("std.gc() hands quick-stack blocks back so churn leaves the arena-only allocator's block count", () => {
std.gc();
const m0 = memoryUsage().mallocCount;
let check = 0;
for (let round = 0; round < 3; round++) {
    let keep = [];
    for (let i = 0; i < 20000; i++) {
        const o = { a: i };
        for (let k = 0; k < (i % 24); k++) o["p" + k] = k;
        keep.push(o, "s" + i, [i, i + 1, i % 7]);
    }
    for (let i = 0; i < keep.length; i += 3) check += keep[i].a + keep[i + 2][2];
    keep = null;
    std.gc();
}
const grew = memoryUsage().mallocCount - m0;
let fail = 0;
if (grew >= 30) {
    fail++;
    print("FAIL: churn left " + grew + " allocator blocks after std.gc() (expected < 30)");
}
if (check !== 3 * 200049997) {
    fail++;
    print("FAIL: values read back from reused blocks are wrong: " + check);
}
print("test_malloc_quick_flush: grew=" + grew + (fail ? " FAILED" : " ok"));
if (fail) throw new Error("test_malloc_quick_flush failed");
});
S.end();
