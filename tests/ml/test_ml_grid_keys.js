// ml.gridSearch read each grid key through JS_ToCString and never freed the C
// string (one leaked string per key per call; a NULL key on failure), and a
// throwing grid getter's error was replaced by a generic TypeError. Found by
// the dyna:ml input sweep under LeakSanitizer; the san stage runs this suite.
import * as ml from "dyna:ml";
let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
async function aerr(f) { try { await f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const X = [[0], [1], [2], [3], [4], [5], [6], [7]], y = [0, 1, 0, 1, 0, 1, 0, 1];
const factory = (p) => new ml.LogisticRegression(p);
const r = await ml.gridSearch(factory, X, y, { C: [0.1, 1], maxIter: [50, 100] }, { k: 2 });
eq(r.results.length, 4, "a two-key grid has 2x2 points (control)");
const grid = { C: [1] };
Object.defineProperty(grid, "maxIter", { enumerable: true, get() { throw new RangeError("grid getter threw"); } });
eq(await aerr(() => ml.gridSearch(factory, X, y, grid, { k: 2 })), "RangeError: grid getter threw", "a throwing grid getter's own error propagates");
eq(await aerr(() => ml.gridSearch(factory, X, y, { C: 3 }, { k: 2 })), "TypeError: dyna:ml every grid value must be an array", "a non-array grid value (control)");

print("test_ml_grid_keys: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
