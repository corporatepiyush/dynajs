// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["ret@1"];
__EXP[1] = ["b0,f0,b1,f1,f2"];
__EXP[2] = ["ff0", "ff1"];
// B: loop var read in a finally block
function run() {
  let log = [];
  for (let i = 0; i < 3; i++) {
    try {
      log.push("try" + i);
      if (i === 1) return "ret@" + i;
    } finally {
      log.push("fin" + i);
    }
  }
  return "no-ret";
}
__L(0, run());
let out = [];
for (let j = 0; j < 4; j++) {
  try {
    if (j === 2) break;
    out.push("b" + j);
  } finally {
    out.push("f" + j);
  }
}
__L(1, out.join(","));
function f2() {
  for (let k = 0; k < 3; k++) {
    try {
      if (k === 1) throw new Error("x" + k);
    } finally {
      __L(2, "ff" + k);
    }
  }
}
try { f2(); } catch (e) { __A("b04_tdz_finally.js:caught", function () { assert_eq(e.message, "x1", "caught"); }); }
let s = 0;
for (let m = 0; m < 3; m++) {
  try { s += m; } finally { m += 0; }
}
__A("b04_tdz_finally.js:mut", function () { assert_eq(s, 3, "mut"); });
// finally MUTATES the loop var — loop must honor it
let log2 = [];
for (let n = 0; n < 3; n++) {
  log2.push(n);
  try { } finally { n++; }
}
__A("b04_tdz_finally.js:mut2", function () { assert_eq(log2.join(","), "0,2", "mut2"); });
// return value computed before finally runs, finally can override
function ov() {
  for (let i = 0; i < 2; i++) {
    try { return "from-try"; } finally { if (i === 1) { } }
  }
  return "after";
}
__A("b04_tdz_finally.js:ov1", function () { assert_eq(ov(), "from-try", "ov1"); });
function ov2() {
  try { return "a"; } finally { return "b"; }
}
__A("b04_tdz_finally.js:ov2", function () { assert_eq(ov2(), "b", "ov2"); });

summary("bbreview");
