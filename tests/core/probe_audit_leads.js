// flags: --std
import * as std from "std";

const R = [];   // [lead, observed]; compared against EXPECTED at the end
const chk = (n, f) => {
  try { R.push([n, String(f())]); }
  catch (e) { R.push([n, "THREW: " + String(e && e.message).slice(0, 64)]); }
};
const mod = (name) => { try { return std.loadFile ? null : null; } catch { return null; } };

chk("Date max toISOString", () => new Date(8.64e15).toISOString());
chk("Date max toUTCString", () => new Date(8.64e15).toUTCString());
chk("Date min toISOString", () => new Date(-8.64e15).toISOString());
chk("Date.parse leading pad", () => {
  const b = "Thu, 01 Jan 1970 00:00:00 GMT";
  const p = " ".repeat(120) + b;
  return "plain=" + Date.parse(b) + " padded=" + Date.parse(p);
});
chk("toFixed(100) length", () => (1.5).toFixed(100).length);
chk("toPrecision(100) length", () => (1.5).toPrecision(100).length);
chk("numeric atom >= 2^31", () => {
  const o = {};
  o["2147483648"] = "a"; o["4294967296"] = "b";
  return "keys=" + JSON.stringify(Object.keys(o)) + " get=" + o["2147483648"] + "," + o["4294967296"];
});
chk("JSON.parse __proto__", () => {
  const o = JSON.parse('{"__proto__":{"x":1}}');
  return Object.getPrototypeOf(o) === Object.prototype ? "ok proto intact" : "RETARGETED";
});
chk("regexp test() arg coercion order", () => {
  const order = [];
  const re = /a/;
  re.exec = function () { order.push("exec"); return null; };
  try { re.test({ toString() { order.push("toString"); return "a"; } }); }
  catch (e) { order.push("threw"); }
  return order.join(",") || "(none)";
});
chk("unicode script property", () => /\p{Script=Latin}/u.test("a") + "," + /\p{Script=Greek}/u.test("a"));

chk("sortBy key ordering", () => {
  if (typeof [].sortBy !== "function") return "n/a";
  return [{ k: "b b" }, { k: "b a" }, { k: "a z" }].sortBy("k").map(x => JSON.stringify(x.k)).join(",");
});
chk("groupBy __proto__ key", () => {
  if (typeof [].groupBy !== "function") return "n/a";
  const r = [1].groupBy(() => "__proto__");
  return Object.getPrototypeOf(r) === Object.prototype ? "ok proto intact" : "RETARGETED";
});
chk("BigInt.asUintN sign", () => {
  let neg = 0;
  for (let b = 1; b <= 200; b++) if (BigInt.asUintN(b, -1n) < 0n) neg++;
  return neg ? "NEGATIVE x" + neg : "ok";
});

function withMod(name, fn, label) {
  chk(label, () => {
    let m = null;
    try { m = globalThis.__mods && globalThis.__mods[name]; } catch { }
    if (!m) return "module not loaded";
    return fn(m);
  });
}


// ============================================================================
// PINNED VERDICT (B1-13). This probe used to wrap every lead in try/catch,
// print the results and exit 0 unconditionally: with one binary in the gate it
// could not fail, so it decorated the core stage instead of testing anything.
// Every lead now has an EXPECTED value; a mismatch prints FAIL and the process
// exits nonzero, so a behaviour change in Date formatting, numeric-atom
// ordering, proto retargeting, sortBy ordering or the regexp coercion order
// turns the stage red.
//
// Provenance: captured from the release build at workspace base (branch
// audit-hardening). tests/pin_oracle_verdicts.py regenerates the pins for the
// oracle_* files; these are edited by hand and reviewed line by line.
const EXPECTED = {
  "Date max toISOString": "+275760-09-13T00:00:00.000Z",
  "Date max toUTCString": "Sat, 13 Sep 275760 00:00:00 GMT",
  "Date min toISOString": "-271821-04-20T00:00:00.000Z",
  "Date.parse leading pad": "plain=0 padded=0",
  "toFixed(100) length": "102",
  "toPrecision(100) length": "101",
  "numeric atom >= 2^31": "keys=[\"2147483648\",\"4294967296\"] get=a,b",
  "JSON.parse __proto__": "ok proto intact",
  "regexp test() arg coercion order": "exec",
  "unicode script property": "true,false",
  "sortBy key ordering": "\"a z\",\"b a\",\"b b\"",
  "groupBy __proto__ key": "ok proto intact",
  "BigInt.asUintN sign": "ok"
};
let probeFail = 0;
for (const [n, v] of R) {
    const want = EXPECTED[n];
    if (want === undefined) {
        print("FAIL: new lead '" + n + "' has no pinned expectation -- add one");
        probeFail++;
    } else if (v !== want) {
        print("FAIL: " + n + "\n      got:  " + v + "\n      want: " + want);
        probeFail++;
    } else {
        print(n.padEnd(30) + " => " + v);
    }
}
for (const n of Object.keys(EXPECTED)) {
    if (!R.some((r) => r[0] === n)) {
        print("FAIL: pinned lead '" + n + "' no longer runs -- it was deleted or renamed");
        probeFail++;
    }
}
if (R.length !== Object.keys(EXPECTED).length && probeFail === 0)
    print("FAIL: lead count " + R.length + " vs pinned " + Object.keys(EXPECTED).length);
print("--- probe complete: " + R.length + " leads, " +
      (probeFail ? probeFail + " MISMATCHED the pin" : "all match their pins") + " ---");
std.exit(probeFail ? 1 : 0);
