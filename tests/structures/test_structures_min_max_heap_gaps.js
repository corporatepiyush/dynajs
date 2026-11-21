// covers: dyna:structures.MinMaxHeap
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_min_max_heap_gaps");
import { BiMap, MinMaxHeap, Multiset, RangeSet } from "dyna:structures";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }
const J = (v) => JSON.stringify(v);

__kitS.section("peeking an empty heap is undefined, not a throw", () => {
  const h = new MinMaxHeap();
  check(h.peekMin() === undefined && h.peekMax() === undefined,
        "peeking an empty heap is undefined, not a throw");
  for (const x of [5, 1, 9, 3, 7]) h.push(x);
  check(h.peekMin() === 1, "peekMin is the smallest, got " + h.peekMin());
  check(h.peekMax() === 9, "peekMax is the largest, got " + h.peekMax());
  check(h.size === 5, "and neither peek consumes, got size " + h.size);
  h.popMax();
  check(h.peekMax() === 7, "peekMax follows a popMax, got " + h.peekMax());
  h.popMin();
  check(h.peekMin() === 3, "peekMin follows a popMin, got " + h.peekMin());
  const one = new MinMaxHeap();
  one.push(42);
  check(one.peekMin() === 42 && one.peekMax() === 42,
        "a single element is both the min and the max");
});
__kitS.end();

if (fails === 0) print("test_structures_gaps: all " + n + " checks passed");
else print("test_structures_gaps: " + fails + " FAILED of " + n);
