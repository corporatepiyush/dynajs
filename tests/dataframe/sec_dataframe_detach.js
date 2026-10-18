// flags: --std
import * as std from "std";
import { DataFrame } from "dyna:dataframe";
let fail = 0;
function check(name, cond, detail) {
  if (!cond) { fail++; console.log("FAIL", name, detail ?? ""); }
}

// PENTEST: the coerce-before-resolve contract.
//
// The module's standing rule: every argument is coerced by code that can run
// arbitrary JS (valueOf/toString/proxy traps), and that JS can DETACH the very
// buffer a method is about to read. So coercion must complete BEFORE any column
// pointer is bound, and a buffer detached mid-coercion must fail cleanly --
// never read freed memory.
//
// The engine's own way to detach is ArrayBuffer.prototype.transfer. A frame
// built directly from a transferable buffer can have that buffer detached by a
// hostile coercion, so the method must refuse with a clean TypeError.

// A hostile scalar whose valueOf detaches `buf` then returns a value.
function hostileDetach(buf, fire) {
  return new Proxy({}, {
    get(t, prop) {
      if (prop === Symbol.toPrimitive || prop === "valueOf" || prop === "toString") {
        if (fire) fire();
        buf.transfer();
        return () => 42;
      }
      return undefined;
    },
  });
}

function makeFrame() {
  const buf = new ArrayBuffer(8 * 64);
  const v = new Float64Array(buf);
  for (let i = 0; i < 64; i++) v[i] = i;
  return { df: new DataFrame({ k: new Int32Array(64), v }), buf };
}

// Each method must throw cleanly (not crash) when its scalar coercion detaches
// the source buffer mid-call. We count a crash as a failure.
let crashed = 0;
function exercise(name, fn) {
  let outcome = "returned";
  try { fn(); }
  catch (e) { outcome = "threw " + e.name; }
  check(name + " threw instead of returning (" + outcome + ")", outcome !== "returned", "");
  if (outcome === "returned" || /crash|SEGV|fatal/.test(String(outcome))) crashed++;
}

// RESAMPLE: the interval is coerced (valueOf) after the time column is named.
exercise("RESAMPLE interval detach", () => {
  const buf = new ArrayBuffer(8 * 64);
  const t = new Float64Array(buf);
  for (let i = 0; i < 64; i++) t[i] = i;
  const ts = new DataFrame({ t });
  ts.RESAMPLE("t", hostileDetach(buf));
});

// SAMPLE: the seed is coerced (valueOf) after n.
exercise("SAMPLE seed detach", () => {
  const { df, buf } = makeFrame();
  df.SAMPLE(2, hostileDetach(buf));
});

// MASK: the fill is coerced (valueOf) after the mask.
exercise("MASK fill detach", () => {
  const { df, buf } = makeFrame();
  df.MASK(new Uint8Array(64).fill(1), hostileDetach(buf));
});

// JOIN: the `how` is coerced (toString) after both keys resolve.
exercise("JOIN how detach", () => {
  const { df, buf } = makeFrame();
  const other = new DataFrame({ k: Int32Array.from([1]), v: Float64Array.from([1]) });
  df.JOIN(other, "k", "k", hostileDetach(buf));
});

// PIVOT: the agg is coerced (toString) after the columns resolve.
exercise("PIVOT agg detach", () => {
  const { df, buf } = makeFrame();
  df.PIVOT("k", "k", "v", hostileDetach(buf));
});

// JSON_AGG: the mask argument is coerced via df_mask_arg's buffer path.
exercise("JSON_AGG mask detach", () => {
  const { df, buf } = makeFrame();
  df.JSON_AGG("k", "v", hostileDetach(buf));
});

// A genuinely detached buffer (before the call) must throw a clean TypeError
// naming the column, on every family.
{
  const buf = new ArrayBuffer(8 * 64);
  const v = new Float64Array(buf);
  const df = new DataFrame({ k: new Int32Array(64), v });
  buf.transfer();
  const msgs = [];
  for (const [name, fn] of [
    ["SUM", () => df.SUM("v")],
    ["FILTER", () => df.FILTER(new Uint8Array(64))],
    ["COPY", () => df.COPY()],
  ]) {
    try { fn(); check("detached " + name + " throws", false); }
    catch (e) { msgs.push(name + ":" + e.name); }
  }
  check("detached frame methods throw cleanly", msgs.every((m) => m.endsWith(":TypeError")), msgs.join(" "));
}

check("crashed === 0", crashed === 0, "crashed=" + crashed);
console.log(fail === 0 ? "SECURITY PROBES PASS" : fail + " FAILURES");
if (fail > 0) std.exit(1);
