/* test_structures_gaps.js -- the nine dyna:structures methods no test named.
 *
 * Found by listing every prototype and static method of every exported class
 * (881 of them) and grepping the test suite for each: 872 were referenced
 * somewhere, and these nine were referenced nowhere. That is the same sweep
 * that turned up App.upload having no test at all.
 *
 * All nine behave as documented, so this
 * pins behaviour rather than fixing it. The cases that matter are the ones the
 * documentation makes a promise about: BiMap's uniqueness in BOTH directions
 * and what happens when that is violated, and RangeSet's encloses-vs-intersects
 * distinction, which is easy to implement as the same predicate by accident.
 */
import { BiMap, MinMaxHeap, Multiset, RangeSet } from "dyna:structures";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }
const J = (v) => JSON.stringify(v);

/* ---- BiMap: forceSet / hasValue / deleteValue / inverseEntries ---------- */
{
  const b = new BiMap();
  b.set("a", "1"); b.set("b", "2");

  check(b.hasValue("1") === true,  "hasValue finds a bound value");
  check(b.hasValue("zz") === false, "and rejects an unbound one");
  /* Keys and values are STRINGS: the doc says string<->string, so a number
     must coerce rather than miss. A silent miss here is the dangerous shape. */
  const bn = new BiMap();
  bn.set("k", 1);
  check(bn.hasValue(1) === true && bn.hasValue("1") === true,
        "a numeric value coerces to its string form in BOTH lookups");

  check(J(b.inverseEntries()) === J([["1", "a"], ["2", "b"]]),
        "inverseEntries is entries with the pairs swapped, got " + J(b.inverseEntries()));

  /* the uniqueness promise, and the escape hatch from it */
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

  /* rebinding an existing KEY is allowed and frees its old value */
  const r = new BiMap();
  r.set("k", "v1"); r.set("k", "v2");
  check(r.hasValue("v1") === false && r.hasValue("v2") === true,
        "rebinding a key frees the value it used to hold");
}

/* ---- MinMaxHeap: peekMin / peekMax ------------------------------------- */
{
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
  /* one element is both ends at once -- the case an index-juggling bug hits */
  const one = new MinMaxHeap();
  one.push(42);
  check(one.peekMin() === 42 && one.peekMax() === 42,
        "a single element is both the min and the max");
}

/* ---- Multiset: elementSet ---------------------------------------------- */
{
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
}

/* ---- RangeSet: encloses vs intersects ---------------------------------- */
{
  const rs = new RangeSet();
  rs.add(10, 20);

  /* These two are easy to implement as the same predicate by accident, so
     every case below is chosen where the two answers DIFFER. */
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

  /* an inverted/empty interval is a no-op for add(), so ask what these say */
  check(rs.intersects(15, 15) === false, "an empty interval intersects nothing");

  /* across two disjoint ranges: enclosing must not be satisfied by a gap */
  const two = new RangeSet();
  two.add(0, 10); two.add(20, 30);
  check(two.encloses(5, 25) === false,
        "a range spanning the GAP between two ranges is not enclosed");
  check(two.intersects(5, 25) === true, "...though it does intersect");
  check(two.encloses(22, 28) === true, "while a subrange of the second is");
}

if (fails === 0) print("test_structures_gaps: all " + n + " checks passed");
else print("test_structures_gaps: " + fails + " FAILED of " + n);
