__EXP = {};
__EXP[0] = ["with-i,own0,evwith-i,with-i,own1,evwith-i,with-i,own2,evwith-i"];
__EXP[1] = ["h0,h0"];
__EXP[2] = ["0,1,2"];
var log = [];
var shadow = { i: "with-i", j: "with-j" };
for (let i = 0; i < 3; i++) {
  with (shadow) {
    log.push(String(i));
  }
  log.push("own" + i);
  with (shadow) { eval("log.push('ev' + i)"); }
}
__L(0, log.join(","));
var log2 = [];
var holder = { v: 0 };
for (let v = 0; v < 4; v++) {
  with (holder) {
    v = v + 0;
    log2.push("h" + v);
  }
  v += 1;
}
__L(1, log2.join(","));
var fns = [];
for (let c = 0; c < 3; c++) {
  with (shadow) { fns.push(() => c); }
}
__L(2, fns.map(f => f()).join(","));

summary("parser_core_ext");
