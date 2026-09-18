// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[2] = ["detach-mid => 1,2 TypeError"];
__EXP[4] = ["r2 TypeError"];
__EXP[6] = ["r3 TypeError"];
__EXP[7] = ["manual => 9:false T:? T:?"];
__EXP[8] = ["vals => NaN,-0,1.5"];
__EXP[9] = ["content => 1,20,30"];
__EXP[10] = ["reverse-mid => 3,1,3 final=2,1,3"];
__EXP[11] = ["subarray => 1,2,99,98"];
// F10: for-of over typed arrays — buffer detach (transfer) mid-iteration ends it
// Error MESSAGES are engine-specific (dynajs: "ArrayBuffer is detached or resized",
// node: "Cannot perform Array Iterator.prototype.next on a detached ArrayBuffer") —
// we assert e.name only; message text is a documented whitelist divergence.
const out = typeof console !== "undefined" ? console.log : print;
const canTransfer = typeof ArrayBuffer.prototype.transfer === "function";

if (!canTransfer) {
  __L(0, "detach => skip");
} else {
  {
    const u8 = new Uint8Array([1, 2, 3, 4]);
    const seen = [];
    try {
      for (const v of u8) {
        seen.push(v);
        if (seen.length === 2) u8.buffer.transfer();
      }
      __L(1, "detach-mid => " + seen.join(",") + " NO-THROW");
    } catch (e) { __L(2, "detach-mid => " + seen.join(",") + " " + e.name); }
  }
  // manual next() after detach
  {
    const u9 = new Uint8Array([9, 8]);
    const it = u9[Symbol.iterator]();
    const r1 = it.next();
    u9.buffer.transfer();
    let r2, r3;
    try { r2 = it.next(); __L(3, "r2 NO-THROW " + r2.done); }
    catch (e) { __L(4, "r2 " + e.name); r2 = { value: "T", done: "?" }; }
    try { r3 = it.next(); __L(5, "r3 NO-THROW " + r3.done); }
    catch (e) { __L(6, "r3 " + e.name); r3 = { value: "T", done: "?" }; }
    __L(7, "manual => " + r1.value + ":" + r1.done + " " + r2.value + ":" + r2.done + " " + r3.value + ":" + r3.done);
  }
}
// values: NaN / -0 pass through with identity intact
{
  const f64 = new Float64Array([NaN, -0, 1.5]);
  const got = [];
  for (const v of f64) got.push(Object.is(v, -0) ? "-0" : (Number.isNaN(v) ? "NaN" : String(v)));
  __L(8, "vals => " + got.join(","));
}
// mutating CONTENT mid-iteration (not length) is visible to later steps
{
  const u = new Uint8Array([1, 2, 3]);
  const seen = [];
  for (const v of u) {
    seen.push(v);
    u[1] = 20; u[2] = 30;
  }
  __L(9, "content => " + seen.join(","));
}
// sort()/reverse() on the TA mid-iteration reshuffles future reads
{
  const t = new Uint8Array([3, 1, 2]);
  const seen = [];
  for (const v of t) {
    seen.push(v);
    if (seen.length === 1) t.reverse();
  }
  __L(10, "reverse-mid => " + seen.join(",") + " final=" + t.join(","));
}
// subarray view shares the buffer; write through it mid-iteration
{
  const base = new Uint8Array([1, 2, 3, 4]);
  const view = base.subarray(0, 4);
  const seen = [];
  for (const v of base) {
    seen.push(v);
    if (seen.length === 1) { view[2] = 99; view[3] = 98; }
  }
  __L(11, "subarray => " + seen.join(","));
}

summary("arrays_ext");
