// covers: dyna:structures.Multiset
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_multiset_gaps");
import { BiMap, MinMaxHeap, Multiset, RangeSet } from "dyna:structures";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }
const J = (v) => JSON.stringify(v);

__kitS.section("an empty multiset has an empty elementSet", () => {
  const ms = new Multiset();
  check(J(ms.elementSet()) === J([]), "an empty multiset has an empty elementSet");
  ms.add("x"); ms.add("x"); ms.add("x"); ms.add("y");
  check(ms.count("x") === 3, "count reflects the multiplicity, got " + ms.count("x"));
  const es = ms.elementSet().slice().sort();
  check(J(es) === J(["x", "y"]),
        "elementSet is DISTINCT elements, each once regardless of count, got " + J(es));
  ms.remove("x");
  check(J(ms.elementSet().slice().sort()) === J(["x", "y"]),
        "an element with count remaining stays in the set");
  ms.remove("x"); ms.remove("x");
  check(J(ms.elementSet()) === J(["y"]),
        "and leaves it once the count reaches zero, got " + J(ms.elementSet()));
});
__kitS.end();

if (fails === 0) print("test_structures_gaps: all " + n + " checks passed");
else print("test_structures_gaps: " + fails + " FAILED of " + n);
