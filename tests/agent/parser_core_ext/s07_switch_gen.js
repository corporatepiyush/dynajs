// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
// S7: fused switch probes inside generators (switch over for-of elements,
// per-iteration case blocks, yield between fused checks).
function* g() {
  for (const t of ["x", 1, 2, null, undefined, NaN]) {
    switch (t) {
      case "x": yield "sx"; break;
      case 1: yield "n1"; break;
      case 2: yield "n2"; break;
      case NaN: yield "nan-never"; break;
      default: yield "d:" + String(t);
    }
  }
}
__A("s07_switch_gen.js:a", function () { assert_eq([...g()].join(","), "sx,n1,n2,d:null,d:undefined,d:NaN", "a"); });
// switch on the generator's own resume state via closure var
function* g2() {
  var state = 0;
  for (;;) {
    switch (state) {
      case 0: state = 1 + 1; yield "a"; break;
      case 2: state = 3; yield "b"; break;
      case 3: return "fin";
      default: state = 3; yield "dd"; break;
    }
  }
}
__A("s07_switch_gen.js:b", function () { assert_eq([...g2()].join(","), "a,b", "b"); });
// fused string switch inside a generator resumed by two consumers alternately
function* g3() {
  for (const w of ["aa", "bb", "cc", "dd"]) {
    switch (w) {
      case "aa": yield 1; break;
      case "bb": yield 2; break;
      case "cc": yield 3; break;
      default: yield 4;
    }
  }
}
var it = g3(), out = [];
out.push(it.next().value, it.next().value, it.next().value, it.next().value, JSON.stringify(it.next()));
__A("s07_switch_gen.js:c", function () { assert_eq(out.join(","), "1,2,3,4,{\"done\":true}", "c"); });

summary("parser_core_ext");
