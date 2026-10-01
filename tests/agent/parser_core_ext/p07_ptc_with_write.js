__EXP = {};
__EXP[0] = ["a 4 4"];
__EXP[1] = ["~deep"];
__EXP[2] = ["~deep"];
__EXP[3] = ["b 10 10"];
__REQ = {"dynajs": {"0": 1, "1": 1, "3": 1}, "node": {"0": 1, "2": 1, "3": 1}};
var obj = { counter: 0 };
function rec(n) {
  with (obj) {
    counter = counter + 1;
    if (n === 0) return counter;
  }
  return rec(n - 1);
}
__L(0, "a", rec(3), obj.counter);
obj.counter = 0;
try { __L(1, "deep", rec(250000), obj.counter); } catch (e) { __L(2, "deep", e.constructor.name, obj.counter); }
var obj2 = { c: 0 };
function rec2(n) {
  with (obj2) {
    c = c + 2;
    if (n === 0) return c;
    return rec2(n - 1);
  }
}
__L(3, "b", rec2(4), obj2.c);
var glob = { tag: "G" };
function rec3(n) {
  with (glob) {
    if (n === 0) return eval("tag");
  }
  return rec3(n - 1);
}
__A("p07_ptc_with_write.js:c", function () { assert_eq(rec3(2), "G", "c"); });

summary("parser_core_ext");
