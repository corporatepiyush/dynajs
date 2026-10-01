__EXP = {};
__EXP[0] = ["a o1 o2 d zero zero d d d"];
__EXP[1] = ["b fr1 fr2 d"];
__EXP[2] = ["c 0 d 0"];
__EXP[4] = ["d miss 0"];
var o1 = { id: 1 }, o2 = { id: 1 };
function f(x) {
  switch (x) {
    case o1: return "o1";
    case o2: return "o2";
    case NaN: return "nan";
    case 0: return "zero";
    case -0: return "negzero";
    default: return "d";
  }
}
__L(0, "a", f(o1), f(o2), f({ id: 1 }), f(0), f(-0), f(NaN), f("0"), f(true));
var fr1 = Object.freeze({ v: 9 });
var fr2 = Object.freeze({ v: 9 });
function f2(x) { switch (x) { case fr1: return "fr1"; case fr2: return "fr2"; default: return "d"; } }
__L(1, "b", f2(fr1), f2(fr2), f2(Object.freeze({ v: 9 })));
var reads = 0;
var vo = { valueOf() { reads++; return 5; } };
function f3(x) {
  switch (x) {
    case 1: return "c1";
    case 5: return "c5";
    case "5": return "s5";
    default: return "d";
  }
}
__L(2, "c", reads, f3(vo), reads);
var reads2 = 0;
var disc = { valueOf() { reads2++; return 5; } };
switch (disc) {
  case 5: __L(3, "d", "hit", reads2); break;
  default: __L(4, "d", "miss", reads2);
}

summary("parser_core_ext");
