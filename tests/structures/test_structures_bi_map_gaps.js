// covers: dyna:structures.BiMap
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_bi_map_gaps");
import { BiMap, MinMaxHeap, Multiset, RangeSet } from "dyna:structures";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }
const J = (v) => JSON.stringify(v);

__kitS.section("hasValue finds a bound value", () => {
  const b = new BiMap();
  b.set("a", "1"); b.set("b", "2");

  check(b.hasValue("1") === true,  "hasValue finds a bound value");
  check(b.hasValue("zz") === false, "and rejects an unbound one");
  const bn = new BiMap();
  bn.set("k", 1);
  check(bn.hasValue(1) === true && bn.hasValue("1") === true,
        "a numeric value coerces to its string form in BOTH lookups");

  check(J(b.inverseEntries()) === J([["1", "a"], ["2", "b"]]),
        "inverseEntries is entries with the pairs swapped, got " + J(b.inverseEntries()));

  let threw = null;
  try { b.set("c", "2"); } catch (e) { threw = e.constructor.name; }
  check(threw === "TypeError",
        "set() REFUSES a value already bound to another key, got " + threw);
  check(b.get("b") === "2" && b.get("c") === undefined,
        "and the refused set changed nothing");

  const ret = b.forceSet("c", "2");
  check(ret === b, "forceSet returns `this` for chaining");
  check(b.get("c") === "2", "forceSet binds the new key");
  check(b.get("b") === undefined,
        "and EVICTS the pair that owned the value -- that is the whole point");
  check(b.keyOf("2") === "c", "the inverse follows the eviction");

  check(b.deleteValue("2") === true, "deleteValue removes by value");
  check(b.deleteValue("2") === false, "and reports false the second time");
  check(b.get("c") === undefined && b.keyOf("2") === undefined,
        "leaving neither direction behind");

  const r = new BiMap();
  r.set("k", "v1"); r.set("k", "v2");
  check(r.hasValue("v1") === false && r.hasValue("v2") === true,
        "rebinding a key frees the value it used to hold");
});
__kitS.end();

if (fails === 0) print("test_structures_gaps: all " + n + " checks passed");
else print("test_structures_gaps: " + fails + " FAILED of " + n);
