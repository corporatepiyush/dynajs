// flags: --std
// covers: c:src/mm/js_malloc.inc.c
// Object create and free take an inline quick-list path (js_malloc_rt_fast / js_free_rt_fast) for the JSObject block and
// its property array: a small request pops the size class's quick list, a small block is pushed back while the list is
// under JS_MALLOC_QUICK_MAX, and everything else (empty request, large block, sanitizer build with pools off) goes
// through the generic allocator. The cases pin that property arrays on both sides of the small/large boundary (31 and
// 32+ slots of 16 bytes) keep their values across interleaved create/free, that blocks recycled from the quick list
// never alias a live object, that dictionary-mode growth reallocates correctly after a quick-list pop, and that
// repeated create/free rounds plateau in malloc size instead of growing.
import * as std from "std";
import { memoryUsage } from "dyna:sys";
import { sections } from "../lib/kit.js";
let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }
const S = sections("test_malloc_quick_inline");
const mk = (n, tag) => { const o = {}; for (let i = 0; i < n; i++) o["p" + i] = tag * 1000 + i; return o; };
const check = (o, n, tag) => { for (let i = 0; i < n; i++) if (o["p" + i] !== tag * 1000 + i) return false; return Object.keys(o).length === n; };
S.section("sizes across the small/large boundary", () => {
    for (const n of [0, 1, 2, 7, 8, 15, 16, 29, 30, 31, 32, 33, 40, 64, 100]) {
        let good = true;
        const keep = [];
        for (let t = 0; t < 200; t++) { const o = mk(n, t); if (!check(o, n, t)) good = false; if (t & 1) keep.push([o, t]); }
        for (const [o, t] of keep) if (!check(o, n, t)) good = false;
        ok(good, "values intact n=" + n);
    }
});
S.section("recycled blocks never alias live objects", () => {
    const live = new Set();
    let good = true;
    for (let r = 0; r < 2000; r++) {
        const a = { r, x: r * 2 }, b = mk(r % 45, r);
        if (live.has(a) || live.has(b)) good = false;
        if (r % 5 === 0) live.add(a); else if (r % 7 === 0) live.add(b);
    }
    for (const o of live) if ("x" in o ? o.x !== o.r * 2 : false) good = false;
    ok(good && live.size > 0, "no aliasing");
});
S.section("growth after quick pop", () => {
    let good = true;
    for (let r = 0; r < 300; r++) {
        const o = { a: r };
        for (let i = 0; i < 80; i++) o["g" + i] = i + r;
        delete o.g3; o.z = r;
        if (o.a !== r || o.g79 !== 79 + r || "g3" in o || o.z !== r) good = false;
    }
    ok(good, "grown objects keep values");
});
S.section("create/free plateau", () => {
    const sizes = [];
    for (let round = 0; round < 6; round++) {
        for (let i = 0; i < 20000; i++) { const o = mk(i % 40, i); if (o.p0 === -1) print(""); }
        std.gc();
        sizes.push(memoryUsage().mallocSize);
    }
    ok(sizes[5] <= sizes[1] + 65536, "plateau " + sizes.join(","));
});
S.end();
print(fail ? "FAIL " + fail + " of " + (pass + fail) : "PASS " + pass);
