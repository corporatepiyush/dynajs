// covers: c:src/builtins/builtin-string.c
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_string_ext_batch8");
let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(a, b, m) { assert(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function threw(fn, Ctor, m) { try { fn(); } catch (e) { assert(!Ctor || e instanceof Ctor, m + " wrong error: " + e); return; } assert(false, m + " did not throw"); }

__kitS.section("startsWith", () => {
eq([1, 2, 3].startsWith([1, 2]), true, "startsWith");
});

__kitS.section("startsWith no", () => {
eq([1, 2, 3].startsWith([2]), false, "startsWith no");
});

__kitS.section("startsWith longer", () => {
eq([1, 2, 3].startsWith([1, 2, 3, 4]), false, "startsWith longer");
});

__kitS.section("startsWith empty", () => {
eq([1, 2, 3].startsWith([]), true, "startsWith empty");
});

__kitS.section("startsWith deep", () => {
eq([[1], [2]].startsWith([[1]]), true, "startsWith deep");
});

__kitS.section("endsWith", () => {
eq([1, 2, 3].endsWith([2, 3]), true, "endsWith");
});

__kitS.section("endsWith no", () => {
eq([1, 2, 3].endsWith([1]), false, "endsWith no");
});

__kitS.section("endsWith empty", () => {
eq([1, 2, 3].endsWith([]), true, "endsWith empty");
});

const mapT = f => xf => ({
  "@@transducer/init": () => xf["@@transducer/init"](),
  "@@transducer/result": a => xf["@@transducer/result"](a),
  "@@transducer/step": (acc, x) => xf["@@transducer/step"](acc, f(x)),
});

__kitS.section("into string", () => {
eq(["a", "b", "c"].into("", mapT(x => x.toUpperCase())), "ABC", "into string");
});
__kitS.end();

print("test_ext_batch8: all " + n + " assertions passed");
