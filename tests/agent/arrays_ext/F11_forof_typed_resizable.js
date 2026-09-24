// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["shrink => 0,0,0"];
__EXP[2] = ["grow => 0,0,0,0,0,0 n=6"];
__EXP[3] = ["to-zero => 0"];
__EXP[4] = ["manual => 0:false 0:false undefined:true"];
// F11: for-of over a typed array on a RESIZABLE ArrayBuffer — resize smaller/larger mid-iteration
const out = typeof console !== "undefined" ? console.log : print;

if (typeof ArrayBuffer.prototype.resize !== "function") {
  __L(0, "resizable => skip");
} else {
  // shrink below the cursor ends iteration
  {
    const rab = new ArrayBuffer(8, { maxByteLength: 32 });
    const u8 = new Uint8Array(rab);
    const seen = [];
    for (const v of u8) {
      seen.push(v);
      if (seen.length === 3) rab.resize(2);
    }
    __L(1, "shrink => " + seen.join(","));
  }
  // grow mid-iteration: iteration continues over the zeroed tail
  {
    const rab2 = new ArrayBuffer(2, { maxByteLength: 16 });
    const u9 = new Uint8Array(rab2);
    const seen2 = [];
    for (const v of u9) {
      seen2.push(v);
      if (seen2.length === 1) rab2.resize(6);
    }
    __L(2, "grow => " + seen2.join(",") + " n=" + seen2.length);
  }
  // shrink to ZERO ends it immediately
  {
    const rab3 = new ArrayBuffer(4, { maxByteLength: 8 });
    const u = new Uint8Array(rab3);
    const seen3 = [];
    for (const v of u) {
      seen3.push(v);
      if (seen3.length === 1) rab3.resize(0);
    }
    __L(3, "to-zero => " + seen3.join(","));
  }
  // manual next() across a resize boundary
  {
    const rab4 = new ArrayBuffer(2, { maxByteLength: 8 });
    const u = new Uint8Array(rab4);
    const it = u[Symbol.iterator]();
    const a = it.next();
    rab4.resize(4);
    const b = it.next();
    rab4.resize(1);
    const c = it.next();
    __L(4, "manual => " + a.value + ":" + a.done + " " + b.value + ":" + b.done + " " + c.value + ":" + c.done);
  }
}

summary("arrays_ext");
