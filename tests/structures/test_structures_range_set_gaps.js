// covers: dyna:structures.RangeSet
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_range_set_gaps");
import { BiMap, MinMaxHeap, Multiset, RangeSet } from "dyna:structures";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }
const J = (v) => JSON.stringify(v);

__kitS.section("encloses: a strict subrange", () => {
  const rs = new RangeSet();
  rs.add(10, 20);

  check(rs.encloses(12, 18) === true,  "encloses: a strict subrange");
  check(rs.encloses(10, 20) === true,  "encloses: the identical range");
  check(rs.encloses(5, 15) === false,  "encloses is FALSE for a partial overlap");
  check(rs.intersects(5, 15) === true, "...where intersects is TRUE");
  check(rs.encloses(15, 25) === false, "encloses is FALSE overlapping the top");
  check(rs.intersects(15, 25) === true, "...where intersects is TRUE");
  check(rs.encloses(0, 30) === false,  "a SUPERrange is not enclosed");
  check(rs.intersects(0, 30) === true, "...but does intersect");

  check(rs.intersects(20, 30) === false,
        "hi is exclusive, so a range starting at hi does not intersect");
  check(rs.intersects(0, 10) === false,
        "and one ending at lo does not either");
  check(rs.intersects(30, 40) === false, "a disjoint range intersects nothing");
  check(rs.encloses(30, 40) === false,   "nor is it enclosed");

  check(rs.intersects(15, 15) === false, "an empty interval intersects nothing");

  const two = new RangeSet();
  two.add(0, 10); two.add(20, 30);
  check(two.encloses(5, 25) === false,
        "a range spanning the GAP between two ranges is not enclosed");
  check(two.intersects(5, 25) === true, "...though it does intersect");
  check(two.encloses(22, 28) === true, "while a subrange of the second is");
});
__kitS.end();

if (fails === 0) print("test_structures_gaps: all " + n + " checks passed");
else print("test_structures_gaps: " + fails + " FAILED of " + n);
