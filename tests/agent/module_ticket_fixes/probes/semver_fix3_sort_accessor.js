// semver_fix3_sort_accessor.js -- FIX 3: dyna:semver sort() write-back must
// follow Set semantics per property shape, matching native Array.prototype.sort
// (expectations generated with node v22: scratch shape matrix):
//   data writable            -> plain write, sorted in place
//   data writable:false      -> TypeError (index also configurable:false/frozen)
//   sealed (data, writable)  -> sorted in place
//   frozen                   -> TypeError
//   accessor getter+setter   -> setter called with the sorted value (was: TypeError)
//   accessor getter-only     -> TypeError
//   mixed data/accessor(gs)  -> setter called once for that slot, data slots written
//   setter-only              -> TypeError (engine reads elements as version
//                               strings; undefined is rejected at read time)
import { sort } from "dyna:semver";
var __pass = 0, __fail = 0;
function expect(msg, got, want) {
  var gs = JSON.stringify(got), ws = JSON.stringify(want);
  if (gs === ws) { __pass++; return; }
  __fail++;
  print("FAIL " + msg + ": got " + gs + " want " + ws);
}
function shapeError(arr, mut) {
  // run with a poisoned Reflect? no -- just return the thrown class name
  try { mut(arr); return null; } catch (e) {
    return (e && e.constructor && e.constructor.name) || String(e);
  }
}

const V = ["1.0.0", "0.1.0", "2.0.0"];

function t_plain() {
  const a = V.slice();
  sort(a);
  expect("plain", Array.from(a), ["0.1.0", "1.0.0", "2.0.0"]);
}
function t_writableFalse() {
  const a = V.slice();
  Object.defineProperty(a, 1, { value: a[1], writable: false, configurable: true });
  expect("writable:false", shapeError(a, sort), "TypeError");
}
function t_sealed() {
  const a = V.slice();
  Object.seal(a);
  const err = shapeError(a, sort);
  expect("sealed err", err, null);
  expect("sealed sorted", Array.from(a), ["0.1.0", "1.0.0", "2.0.0"]);
}
function t_frozen() {
  const a = V.slice();
  Object.freeze(a);
  expect("frozen", shapeError(a, sort), "TypeError");
}
function t_getterSetter() {
  const a = ["", "", ""], log = [];
  for (let i = 0; i < 3; i++) {
    let v = V[i];
    Object.defineProperty(a, i, { get() { return v; }, set(x) { log.push(x); v = x; }, configurable: true });
  }
  const err = shapeError(a, sort);
  expect("getter+setter err", err, null);
  expect("getter+setter setter-observed", log, ["0.1.0", "1.0.0", "2.0.0"]);
  expect("getter+setter view", Array.from(a), ["0.1.0", "1.0.0", "2.0.0"]);
}
function t_getterOnly() {
  const a = ["", "", ""];
  for (let i = 0; i < 3; i++) {
    let v = V[i];
    Object.defineProperty(a, i, { get() { return v; }, configurable: true });
  }
  expect("getter-only", shapeError(a, sort), "TypeError");
}
function t_setterOnly() {
  const a = ["", "", ""];
  for (let i = 0; i < 3; i++) Object.defineProperty(a, i, { set(x) {}, configurable: true });
  expect("setter-only", shapeError(a, sort), "TypeError");
}
function t_mixed() {
  const a = V.slice(), log = [];
  let v = a[1];
  Object.defineProperty(a, 1, { get() { return v; }, set(x) { log.push(x); v = x; }, configurable: true });
  const err = shapeError(a, sort);
  expect("mixed err", err, null);
  expect("mixed setter-observed", log, ["1.0.0"]);
  expect("mixed view", Array.from(a), ["0.1.0", "1.0.0", "2.0.0"]);
}
function t_mixedRO() {
  const a = V.slice();
  Object.defineProperty(a, 0, { value: a[0], writable: false, configurable: true });
  expect("mixed readonly", shapeError(a, sort), "TypeError");
}

expect("sort is function", typeof sort, "function");
t_plain(); t_writableFalse(); t_sealed(); t_frozen();
t_getterSetter(); t_getterOnly(); t_setterOnly(); t_mixed(); t_mixedRO();

print("SUMMARY semver_fix3_sort_accessor pass=" + __pass + " fail=" + __fail + " // 15 expects");
print(__fail === 0 ? "RESULT PASS" : "RESULT FAIL");
if (__fail !== 0) throw new Error("probe failed");
