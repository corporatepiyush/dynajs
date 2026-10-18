// Interpreter fast paths added for interpreter-bound code, each checked against
// an independent reference rather than against the engine's own slow path:
//   bitwise ops on doubles  vs ToInt32/ToUint32 computed with float arithmetic
//                            only (ES2024 7.1.6/7.1.7), never with a bitwise op
//   x ** y on numbers       vs Math.pow (same libm call, the spec's equality)
//   o["literal"]            compiled as o.literal: getters, proxies, primitives,
//                            prototype chains, `this` for calls, null base
//   template literals       ToString per substitution, in order, never
//                            String.prototype.concat (ES2024 13.2.8.6)
let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; if (fails < 30) print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + String(a) + ", want " + String(b)); }

const TWO32 = 4294967296, TWO31 = 2147483648;
function toUint32Ref(x) {
    if (!Number.isFinite(x) || x === 0) return 0;
    const t = Math.trunc(x);
    const m = t - TWO32 * Math.floor(t / TWO32);
    return m;
}
function toInt32Ref(x) { const u = toUint32Ref(x); return u >= TWO31 ? u - TWO32 : u; }

const EDGE = [0, -0, 1, -1, 0.5, -0.5, 1.5, -1.5, 2.5, -2.9, 255.99, 2147483647, 2147483647.9, -2147483648, -2147483648.9,
    2147483648, -2147483649, 4294967295, 4294967296, 4294967297.5, -4294967296, 1e10, -1e10, 1e20, -1e20, 2 ** 53, 2 ** 53 + 2,
    2 ** 63, 1.7976931348623157e308, 5e-324, -5e-324, NaN, Infinity, -Infinity, 0x7fffffff, 0x80000000, 0xffffffff, 12345.678, -98765.4321];
for (const x of EDGE) {
    for (const y of [0, 1, 3, 31, 32, 33, -1, 0.5, 7.9, 2147483648, NaN, -0]) {
        const a = toInt32Ref(x), b = toInt32Ref(y), sh = toUint32Ref(y) % 32;
        eq(x | y, toInt32Ref(a + 0) === a ? (function () { let r = 0; for (let i = 0; i < 32; i++) { const ba = Math.floor(toUint32Ref(a) / 2 ** i) % 2, bb = Math.floor(toUint32Ref(b) / 2 ** i) % 2; if (ba || bb) r += 2 ** i; } return toInt32Ref(r); })() : NaN, `${x} | ${y}`);
        eq(x & y, (function () { let r = 0; for (let i = 0; i < 32; i++) { const ba = Math.floor(toUint32Ref(a) / 2 ** i) % 2, bb = Math.floor(toUint32Ref(b) / 2 ** i) % 2; if (ba && bb) r += 2 ** i; } return toInt32Ref(r); })(), `${x} & ${y}`);
        eq(x ^ y, (function () { let r = 0; for (let i = 0; i < 32; i++) { const ba = Math.floor(toUint32Ref(a) / 2 ** i) % 2, bb = Math.floor(toUint32Ref(b) / 2 ** i) % 2; if (ba !== bb) r += 2 ** i; } return toInt32Ref(r); })(), `${x} ^ ${y}`);
        eq(x << y, toInt32Ref(toUint32Ref(a) * 2 ** sh), `${x} << ${y}`);
        eq(x >> y, Math.floor(a / 2 ** sh), `${x} >> ${y}`);
        eq(x >>> y, Math.floor(toUint32Ref(a) / 2 ** sh), `${x} >>> ${y}`);
    }
}
{
    let s = 0;
    for (let k = 0; k < 100000; k++) s += (k * 0.75) | 0;
    let r = 0;
    for (let k = 0; k < 100000; k++) r += Math.trunc(k * 0.75);
    eq(s, r, "loop of (k * 0.75) | 0 equals Math.trunc");
    let calls = 0;
    const v = { valueOf() { calls++; return 3.7; } };
    eq(v | 0, 3, "object operand still goes through valueOf");
    eq(calls, 1, "valueOf called once");
    eq(5n | 2n, 7n, "BigInt operands unaffected");
    let threw = false; try { 1n | 1; } catch (e) { threw = e instanceof TypeError; } check(threw, "mixed BigInt/Number still throws");
}

for (const x of [0, -0, 1, -1, 2, -2, 0.5, -0.5, 3, 10, 1e10, NaN, Infinity, -Infinity, 1.0000001, 7, -7]) {
    for (const y of [0, -0, 1, -1, 2, 3, 0.5, -0.5, 10, 1e10, NaN, Infinity, -Infinity, 1.5, -3]) {
        eq(x ** y, Math.pow(x, y), `${x} ** ${y}`);
    }
}
eq(2 ** 10, 1024, "int ** int");
check(Number.isInteger(3 ** 2) && 3 ** 2 === 9, "int ** int gives an integral number");
eq(2n ** 64n, 18446744073709551616n, "BigInt ** BigInt");
eq((-8) ** (1 / 3), NaN, "negative base, fractional exponent");
eq(1 ** Infinity, NaN, "1 ** Infinity is NaN");

{
    const o = { f: 1, "content-type": "text/plain", "0": "zero", length: 9 };
    eq(o["f"], 1, "plain read");
    eq(o["content-type"], "text/plain", "hyphenated key");
    eq(o["0"], "zero", "numeric-string key keeps the index path");
    eq(o["length"], 9, "length on a plain object");
    eq("abc"["length"], 3, "string primitive length");
    eq([1, 2, 3]["length"], 3, "array length");
    eq("abc"["toUpperCase"](), "ABC", "method on a primitive keeps `this`");
    const proto = { inherited: "p" };
    const child = Object.create(proto);
    eq(child["inherited"], "p", "prototype chain");
    let gets = 0;
    const g = { get x() { gets++; return this === g; } };
    eq(g["x"], true, "getter sees the receiver");
    eq(gets, 1, "getter runs once");
    const seen = [];
    const px = new Proxy({}, { get(t, k) { seen.push(typeof k + ":" + String(k)); return 42; } });
    eq(px["hello"], 42, "proxy get");
    eq(seen.join(), "string:hello", "proxy trap sees the string key");
    const m = { k: 7, get() { return this.k; } };
    eq(m["get"](), 7, "bracket call binds this");
    let threw = null;
    try { const u = null; u["f"]; } catch (e) { threw = e; }
    check(threw instanceof TypeError, "null base throws TypeError");
    eq(({ __proto__: { z: 5 } })["z"], 5, "__proto__ literal then bracket read");
    eq(o?.["f"], 1, "optional bracket read");
    const und = undefined;
    eq(und?.["f"], undefined, "optional bracket on undefined");
}

{
    const saved = String.prototype.concat;
    String.prototype.concat = function () { return "HIJACKED"; };
    const k = 5;
    eq(`a${k}b`, "a5b", "template does not call String.prototype.concat");
    String.prototype.concat = saved;
    const o = { toString() { return "T"; }, valueOf() { return "V"; } };
    eq(`${o}`, "T", "template uses ToString (toString before valueOf)");
    eq("" + o, "V", "plain + still uses ToPrimitive default");
    eq(`${1}|${-0}|${1.5}|${NaN}|${1e21}|${10n}|${null}|${undefined}|${true}`, "1|0|1.5|NaN|1e+21|10|null|undefined|true", "primitive substitutions");
    eq(`${[1, [2, 3]]}`, "1,2,3", "array substitution");
    const order = [];
    const t = { toString() { order.push("t"); return "1"; } }, u = { toString() { order.push("u"); return "2"; } };
    eq(`${(order.push("a"), t)}-${(order.push("b"), u)}`, "1-2", "two substitutions");
    eq(order.join(), "a,t,b,u", "each substitution is converted before the next is evaluated");
    let threw = false;
    try { `${Symbol("s")}`; } catch (e) { threw = e instanceof TypeError; }
    check(threw, "Symbol substitution throws TypeError");
    const tag = (strs, ...vals) => strs.raw.join("|") + vals.join(",");
    eq(tag`a${1}b${2}c`, "a|b|c1,2", "tagged templates unchanged");
    eq(``, "", "empty template");
    eq(`${""}`, "", "empty substitution");
}

print(`test_interp_fastpaths: ${fails ? fails + " FAILED of" : "all"} ${n} checks${fails ? "" : " passed"}`);
if (fails) throw new Error(fails + " failures");
