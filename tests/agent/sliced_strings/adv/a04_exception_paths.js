/* exceptions from callbacks while slices are mid-operation. */
var s = "abcdefghij".repeat(100);
var t = s.slice(10, 900);
var hits = 0;
try {
  t.replace(/a/g, function (m, off) { hits++; if (hits === 5) throw new RangeError("stop"); return "X"; });
} catch (e) { if (e.message !== "stop") throw e; }
if (hits !== 5) throw new Error("adv: replace hits " + hits);
if (t.length !== 890) throw new Error("adv: slice damaged");
var hits2 = 0;
try {
  t.split("").map(function (c, i) { if (i === 100) throw new TypeError("mid"); return c; });
} catch (e) { hits2 = 1; }
if (!hits2) throw new Error("adv: map throw");
/* iterator return() path via break inside for-of */
var seen = 0;
for (var c of t.slice(5, 500)) { seen++; if (seen === 10) break; }
if (seen !== 10) throw new Error("adv: iter break");
console.log("PASS a04");
