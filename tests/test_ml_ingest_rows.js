// dyna:ml reads number[][] X straight from plain arrays' element storage. The
// shortcut applies only to a dense array whose length equals its element count;
// everything else (a stretched length, holes, non-number cells, array-likes)
// still goes through the property path, with the same refusals and values.
import * as ml from "dyna:ml";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
async function fitMsg(X) { try { await new ml.StandardScaler().fit(X); return "ok"; } catch (e) { return e.message; } }

const stretched = [[1, 2], [3, 4], [5, 6]];
stretched[1].length = 3;
ok(/same length/.test(await fitMsg(stretched)), "a row whose length was stretched past its elements is refused as ragged");
const holey = [[1, 2], [3, 4], [5]];
holey[2].length = 2;
ok(/NaN/.test(await fitMsg(holey)), "a trailing hole reads as missing data, not as a short row");
const objCell = [[1, 2], [3, { valueOf() { return 4; } }]];
ok((await fitMsg(objCell)) === "ok", "a cell with valueOf still coerces through the property path");
const mixed = [[1, 2.5], [3, 4], [-0, 1e30]];
const s = new ml.StandardScaler();
await s.fit(mixed);
const flat = new ml.StandardScaler();
await flat.fit(Float64Array.from(mixed.flat()), 3, 2);
const a = await s.transform([[2, 3]]), b = await flat.transform([[2, 3]]);
ok(JSON.stringify(Array.from(a.flat ? a.flat() : a)) === JSON.stringify(Array.from(b.flat ? b.flat() : b)),
   "int and double cells read from plain arrays fit the same model as the same values in a Float64Array");

print("test_ml_ingest_rows: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_ml_ingest_rows failed");
