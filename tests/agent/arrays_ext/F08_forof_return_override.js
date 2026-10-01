__EXP = {};
__EXP[0] = ["on-break => calls=1"];
__EXP[1] = ["on-complete => calls=0"];
__EXP[2] = ["on-body-throw => RangeError calls=1"];
__EXP[4] = ["nonobject => TypeError"];
__EXP[6] = ["throwret => EvalError"];
__EXP[7] = ["nested => o1 i10 o2 i10 calls=2"];
__EXP[8] = ["typed-shared => calls=1"];
__EXP[9] = ["lookup-site => undefined function"];
__EXP[10] = ["restored => undefined"];
const out = typeof console !== "undefined" ? console.log : print;
const ip = Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()));
const origReturn = Object.getOwnPropertyDescriptor(ip, "return");
function restore() {
  if (origReturn) Object.defineProperty(ip, "return", origReturn);
  else delete ip.return;
}
let calls = 0;
try {
  ip.return = function () { calls++; return { value: 1, done: false }; };
  { for (const v of [1, 2, 3]) break; }
  __L(0, "on-break => calls=" + calls);
  calls = 0;
  { for (const v of [1, 2, 3]) {} }
  __L(1, "on-complete => calls=" + calls);
  calls = 0;
  try { for (const v of [1, 2, 3]) { throw new RangeError("body"); } }
  catch (e) { __L(2, "on-body-throw => " + e.name + " calls=" + calls); }
  ip.return = function () { return 42; };
  try { for (const v of [1]) break; __L(3, "nonobject => NO-THROW"); }
  catch (e) { __L(4, "nonobject => " + e.name); }
  ip.return = function () { throw new EvalError("R"); };
  try { for (const v of [1]) break; __L(5, "throwret => NO-THROW"); }
  catch (e) { __L(6, "throwret => " + e.name); }
  ip.return = function () { calls++; return { done: true }; };
  calls = 0;
  const log = [];
  for (const v of [1, 2]) {
    log.push("o" + v);
    for (const w of [10, 20]) { log.push("i" + w); break; }
  }
  __L(7, "nested => " + log.join(" ") + " calls=" + calls);
  calls = 0;
  for (const v of new Uint8Array([1, 2])) break;
  __L(8, "typed-shared => calls=" + calls);
  __L(9, "lookup-site => " + (typeof Array.prototype.return) + " " + (typeof ip.return));
} finally { restore(); }
__L(10, "restored => " + (typeof ip.return));

summary("arrays_ext");
