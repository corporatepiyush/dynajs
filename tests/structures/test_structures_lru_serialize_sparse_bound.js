// covers: dyna:structures.LRU
// tests/structures/test_serialize_sparse_bound.js -- SEC-6 (B16): the value
// serializer behind Worker.postMessage and the dyna:structures serialize()
// methods writes an array as `length` entries, one per index, holes as
// undefined. A sparse array of length 2^32-1 with one element used to cost
// about 115 s and 3.2 GB before anything refused it. Holes beyond the real
// elements are now capped at 2^24: past that serialize throws RangeError at
// once. Arrays within the cap round-trip with their holes as undefined.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_lru_serialize_sparse_bound");
import { LRU } from "dyna:structures";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0, failures = 0;
function check(cond, msg) { n++; if (!cond) { failures++; print("  FAIL:", msg); } }

function roundTrip(v) {
    const c = new LRU(4);
    c.put("k", v);
    return LRU.deserialize(c.serialize()).get("k");
}
function refused(v, label) {
    const t0 = cpuNow();
    try {
        roundTrip(v);
        check(false, label + ": serialized instead of refusing");
    } catch (e) {
        check(e instanceof RangeError && /sparse array of length/.test(e.message), label + ": RangeError, got " + e);
    }
    check(cpuNow() - t0 < 2000, label + ": refused before writing the holes");
}

__kitS.section("length 2^32-1, one element", () => {
    const a = []; a.length = 2 ** 32 - 1; a[5] = 1;
    refused(a, "length 2^32-1, one element");
    const b = []; b.length = 2 ** 24 + 2; b[0] = 1;
    refused(b, "2^24+1 holes, one element");
    refused(new Array(2 ** 24 + 1), "new Array(2^24+1), no elements");
    const sparseObj = Object.assign([], { 3: "x" }); sparseObj.length = 2 ** 30;
    refused(sparseObj, "slow array of length 2^30");
});

__kitS.section("1000-long sparse array round-trips", () => {
    const a = []; a[0] = "first"; a[999] = "last";
    const back = roundTrip(a);
    check(back.length === 1000 && back[0] === "first" && back[999] === "last" && back[500] === undefined, "1000-long sparse array round-trips");
    const edge = []; edge.length = 2 ** 24 + 1; edge[0] = 1;
    check(roundTrip(edge).length === 2 ** 24 + 1, "exactly 2^24 holes is still written");
    const dense = Array.from({ length: 100000 }, (_, i) => i);
    const d2 = roundTrip(dense);
    check(d2.length === 100000 && d2[99999] === 99999, "a dense array is unaffected");
});
__kitS.end();

print("test_serialize_sparse_bound: " + n + " checks, " + failures + " failures");
if (failures) throw new Error("test_serialize_sparse_bound: " + failures + " failures");
