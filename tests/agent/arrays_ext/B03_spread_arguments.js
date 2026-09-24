// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["strict => object|3|1|2|3"];
__EXP[1] = ["strict2 => object|3|1|2|3"];
__EXP[2] = ["strict-empty => object|0|||"];
__EXP[3] = ["spread-args => [9,8,7] [9,8,7] len=3"];
__EXP[4] = ["count => 7"];
__EXP[5] = ["sloppy-map => 42:[42]:true"];
__EXP[6] = ["strict-map => 1:[42]:true"];
__EXP[7] = ["arrow => [4,5,6]"];
__EXP[8] = ["ctor => [1,2,3]"];
__EXP[9] = ["bound => 1:2"];
__EXP[10] = ["proxy-apply => trapped:2:pf:7,8"];
// B03: spread into function arguments + spread OF the arguments object
// (file is sloppy-mode: the directive below comes after the const, deliberately testing
// mapped arguments behavior under spread in both sloppy call targets and arrows)
const out = typeof console !== "undefined" ? console.log : print;
"use strict"; // inert here (not first statement) — file is sloppy

function strictF(a, b, c) {
  "use strict";
  return [typeof arguments, arguments.length, a, b, c].join("|");
}
function spreader() {
  const copy = [...arguments];
  const viaPush = [];
  viaPush.push(...arguments);
  return JSON.stringify(copy) + " " + JSON.stringify(viaPush) + " len=" + copy.length;
}
function lenOnly() { return arguments.length; }

// mapped (sloppy) arguments: mutation maps to the parameter, spread snapshots the CURRENT view
function mapper(x) {
  arguments[0] = 42;
  const c = [...arguments];
  return x + ":" + JSON.stringify(c) + ":" + (arguments[0] === 42);
}
// unmapped check inside a strict inner function
function strictMapper(x) {
  "use strict";
  arguments[0] = 42;
  const c = [...arguments];
  return x + ":" + JSON.stringify(c) + ":" + (arguments[0] === 42);
}

__L(0, "strict => " + strictF(...[1, 2, 3]));
__L(1, "strict2 => " + strictF(...[1], ...[2, 3]));
__L(2, "strict-empty => " + strictF(...[], ...[]));
__L(3, "spread-args => " + spreader(9, 8, 7));
__L(4, "count => " + lenOnly(...[1, 2, 3, 4, 5, 6, 7]));
__L(5, "sloppy-map => " + mapper(1));
__L(6, "strict-map => " + strictMapper(1));

// arrow functions capture the enclosing arguments object, spread both
function outer2() {
  const arrow = () => [...arguments];
  return JSON.stringify(arrow());
}
__L(7, "arrow => " + outer2(4, 5, 6));

// spread into class constructor
class Box {
  constructor(...items) { this.items = items; }
}
const box = new Box(...[1], ...[2, 3]);
__L(8, "ctor => " + JSON.stringify(box.items));

// spread into a bound function
function tgt(a, b) { return a + ":" + b; }
const bound = tgt.bind(null, 1);
__L(9, "bound => " + bound(...[2]));

// spread into a Proxy-apply-trapped callable
const pF = new Proxy(function (a, b) { return "pf:" + a + "," + b; }, {
  apply(t, thisArg, args) { return "trapped:" + args.length + ":" + t(...args); },
});
__L(10, "proxy-apply => " + pF(...[7, 8]));

summary("arrays_ext");
