/* probe_audit_leads.js -- turn the src-wide audit's unverified leads into
 * confirmed defects or refutations, from JS, in one run.
 *
 * The audit raised 104 findings but 90 of its refuter agents died on a session
 * limit, so a finding survived by DEFAULT rather than by review. This file is
 * the verification those refuters never did: each case either reproduces the
 * claim or does not, and the output says which. It is not a pass/fail suite --
 * it prints a table and exits 0 -- because its job is to tell you what is real.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/probe_audit_leads.js
 */
import * as std from "std";

const R = [];
const chk = (n, f) => {
  try { R.push([n, String(f())]); }
  catch (e) { R.push([n, "THREW: " + String(e && e.message).slice(0, 64)]); }
};
const mod = (name) => { try { return std.loadFile ? null : null; } catch { return null; } };

/* ---- core language ---- */
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

/* ---- array extensions ---- */
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

/* ---- native modules ---- */
function withMod(name, fn, label) {
  chk(label, () => {
    let m = null;
    try { m = globalThis.__mods && globalThis.__mods[name]; } catch { }
    if (!m) return "module not loaded";
    return fn(m);
  });
}

for (const [n, v] of R) print(n.padEnd(30) + " => " + v);
print("--- probe complete: " + R.length + " leads exercised ---");
std.exit(0);
