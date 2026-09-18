const out = typeof console !== "undefined" ? console.log : print;
const u8 = new Uint8Array([1, 2, 3, 4]);
let stage = "start";
try {
  for (const v of u8) {
    if (v === undefined) { out("hole?"); break; }
    stage = "got" + v;
    if (stage === "got2") { stage = "transferring"; u8.buffer.transfer(); stage = "transferred"; }
  }
  out("loop-ended-normally");
} catch (e) {
  out("threw-at:" + stage + " name=" + e.name);
}
// manual: where does next() throw?
const u9 = new Uint8Array([9, 8]);
const it = u9[Symbol.iterator]();
it.next();
let t2 = "pre-transfer";
try { u9.buffer.transfer(); t2 = "transfer-ok"; } catch (e) { out("transfer-threw:" + e.name); }
try { const r = it.next(); out("next-after-detach => " + r.value + ":" + r.done); }
catch (e) { out("next-threw:" + e.name); }
