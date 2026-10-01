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
const out = typeof console !== "undefined" ? console.log : print;
"use strict";

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

function mapper(x) {
  arguments[0] = 42;
  const c = [...arguments];
  return x + ":" + JSON.stringify(c) + ":" + (arguments[0] === 42);
}
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

function outer2() {
  const arrow = () => [...arguments];
  return JSON.stringify(arrow());
}
__L(7, "arrow => " + outer2(4, 5, 6));

class Box {
  constructor(...items) { this.items = items; }
}
const box = new Box(...[1], ...[2, 3]);
__L(8, "ctor => " + JSON.stringify(box.items));

function tgt(a, b) { return a + ":" + b; }
const bound = tgt.bind(null, 1);
__L(9, "bound => " + bound(...[2]));

const pF = new Proxy(function (a, b) { return "pf:" + a + "," + b; }, {
  apply(t, thisArg, args) { return "trapped:" + args.length + ":" + t(...args); },
});
__L(10, "proxy-apply => " + pF(...[7, 8]));

summary("arrays_ext");
