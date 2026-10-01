__EXP = {};
__EXP[0] = ["a done fin1,fin2,fin3"];
__EXP[1] = ["b done fin1"];
__EXP[2] = ["c done2 ret3,ret3,ret3"];
var events = [];
function* g(n) {
  try { yield 1; }
  finally { events.push("fin" + n); }
}
function rec(n) {
  if (n === 0) return "done";
  for (const x of g(n)) {
    return rec(n - 1);
  }
}
__L(0, "a", rec(3), events.join(","));
events.length = 0;
__L(1, "b", rec(1), events.join(","));
events.length = 0;
var closable = {
  i: 0,
  [Symbol.iterator]() {
    var done = false;
    return {
      next: () => ({ value: this.i++, done: false }),
      return: (v) => { events.push("ret" + this.i); return { value: v, done: true }; },
    };
  },
};
function rec2(n) { if (n === 0) return "done2"; for (const x of closable) { return rec2(n - 1); } }
__L(2, "c", rec2(3), events.join(","));

summary("parser_core_ext");
