// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["A [null,1,2,3,null] [0,1,2,3,9]"];
__EXP[1] = ["B [4,5,4,5] len=4"];
__EXP[2] = ["C [2.5,3.5] 1 isArray=true | subarray-spread=[2.5,3.5]"];
__EXP[3] = ["D [{\"0\":7,\"1\":8,\"2\":9},0] idx=-1 last=1"];
__EXP[4] = ["E 0,9,9 final=[9,9,9]"];
__EXP[5] = ["F 4:1234"];
__EXP[6] = ["G idx=TypeError inc=TypeError spread=TypeError rev=TypeError concat=TypeError shift=TypeError"];
// X06: typed arrays x spread x mutators cross — spread of TA into bulk paths, TA in
// array literals with elisions, TA views after conversions
const out = typeof console !== "undefined" ? console.log : print;

{
  const u = new Uint8Array([1, 2, 3]);
  __L(0, "A " + JSON.stringify([, ...u, , ]) + " " + JSON.stringify([0, ...u, 9]));
}
{
  const u = new Uint8Array([4, 5]);
  const d = [];
  d.push(...u, ...u);
  __L(1, "B " + JSON.stringify(d) + " len=" + d.length);
}
{
  // TA slice/subarray results spread and searched
  const f = new Float64Array([1.5, 2.5, 3.5]);
  const s = Array.prototype.slice.call(f, 1); // generic slice over TA (length-driven)
  __L(2, "C " + JSON.stringify(s) + " " + s.indexOf(3.5) + " isArray=" + Array.isArray(s) +
      " | subarray-spread=" + JSON.stringify([...f.subarray(1)]));
}
{
  // concat TA into array then reverse then search
  const u = new Uint8Array([7, 8, 9]);
  const r = [0].concat(u).reverse();
  __L(3, "D " + JSON.stringify(r) + " idx=" + r.indexOf(8) + " last=" + r.lastIndexOf(0));
}
{
  // for-of over a TA slice WHILE writing through a second view of the same buffer
  const buf = new ArrayBuffer(8);
  const w = new Uint8Array(buf);
  const v = new Uint8Array(buf, 2, 3);
  const seen = [];
  for (const x of v) {
    seen.push(x);
    w.fill(9);
  }
  __L(4, "E " + seen.join(",") + " final=" + JSON.stringify([...v]));
}
{
  // spread of TA into strict function args (arg count = TA length)
  "use strict";
  const u = new Uint8Array([1, 2, 3, 4]);
  const f = (...args) => args.length + ":" + args.join("");
  __L(5, "F " + f(...u));
}
{
  // TA with detached buffer: search + spread + mutators all must throw/return cleanly
  const u = new Uint8Array([1, 2]);
  u.buffer.transfer();
  const probe = (fn) => { try { return String(fn()); } catch (e) { return e.name; } };
  __L(6, "G idx=" + probe(() => u.indexOf(1)) + " inc=" + probe(() => u.includes(1)) +
      " spread=" + probe(() => JSON.stringify([...u])) + " rev=" + probe(() => u.reverse()) +
      " concat=" + probe(() => [].concat(u)) + " shift=" + probe(() => u.shift()));
}

summary("arrays_ext");
