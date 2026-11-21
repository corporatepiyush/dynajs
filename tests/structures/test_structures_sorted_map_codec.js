// covers: dyna:structures.SortedMap
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_sorted_map_codec");
import { SortedSet, SortedMap } from "dyna:structures";
import { CRC32C } from "dyna:hash";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(a === b, m + " -- got " + a + ", want " + b); }

function keysOf(s) { return s.toArray(); }

function roundTrip(build, label, expectBytesUnder) {
    const src = build();
    const rec = src.serialize();
    const back = SortedSet.deserialize(rec);
    const a = keysOf(src), b = keysOf(back);
    eq(b.length, a.length, label + ": key count");
    let bad = -1;
    for (let i = 0; i < a.length; i++)
        if (!Object.is(a[i], b[i])) { bad = i; break; }
    check(bad < 0, label + ": key " + bad + " differs (" +
          (bad >= 0 ? a[bad] + " vs " + b[bad] : "") + ")");
    const rec2 = back.serialize();
    eq(rec2.length, rec.length, label + ": re-encode length");
    let same = true;
    for (let i = 0; i < rec.length; i++) if (rec[i] !== rec2[i]) { same = false; break; }
    check(same, label + ": re-encode is byte-identical");
    if (expectBytesUnder !== undefined)
        check(rec.length < expectBytesUnder,
              label + ": expected under " + expectBytesUnder + " bytes, got " + rec.length);
    return rec;
}

const N = 20000;
const RAW = 8 * N + 28;

__kitS.section("SortedMap size", () => {
    const m = new SortedMap();
    for (let i = 0; i < 5000; i++) m.set(i, "v" + i);
    const back = SortedMap.deserialize(m.serialize());
    eq(back.size, m.size, "SortedMap size");
    let ok = true;
    for (let i = 0; i < 5000; i++) if (back.get(i) !== "v" + i) { ok = false; break; }
    check(ok, "every SortedMap value survives a compressed-key record");
    const m2 = new SortedMap();
    const vals = [null, true, false, 0, -0, 1.5, "s", NaN];
    for (let i = 0; i < vals.length; i++) m2.set(i, vals[i]);
    const b2 = SortedMap.deserialize(m2.serialize());
    for (let i = 0; i < vals.length; i++)
        check(Object.is(b2.get(i), vals[i]),
              "SortedMap value " + i + ": got " + b2.get(i) + " want " + vals[i]);
});
__kitS.end();

if (fails === 0) print("test_structures_sorted_codec: all " + n + " checks passed");
else print("test_structures_sorted_codec: " + fails + " FAILED of " + n);
