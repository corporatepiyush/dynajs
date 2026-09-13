// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["destr k1 v1", "destr k2 v2", "destr k3 v3"];
__EXP[1] = ["objd 1 2", "objd 3 4"];
__EXP[3] = ["args 2,4,6 "];
__EXP[4] = ["rope 20 20 a t abcdefghijklmnopqrst"];
__EXP[6] = ["rope2 22 22 wxyzwxyzwxyzwxyzwxyzwx"];
// D: destructuring, break-then-resume sharing, arguments, rope strings
const pairs = [["k1", "v1"], ["k2", "v2"], ["k3", "v3"]];
for (const [k, v] of pairs) __L(0, "destr", k, v);
for (const { a: x, b: y } of [{ a: 1, b: 2 }, { a: 3, b: 4 }]) __L(1, "objd", x, y);
const src = [1, 2, 3, 4, 5];
const it = src[Symbol.iterator]();
let got = [];
for (const v of { [Symbol.iterator]: () => it }) {
  got.push(v);
  if (v === 2) break;
}
got.push("r" + it.next().value, "d" + it.next().done);
__A("d05_forof_misc.js:share", function () { assert_eq(got.join(","), "1,2,r3,dfalse", "share"); });
function f() {
  const out = [];
  for (const v of arguments) out.push(v * 2);
  return out.join(",");
}
__L(3, "args", f(1, 2, 3), f());
let s = "abc";
s += "defgh";
s = s + "ij" + "klm";
s += "nopqrst";
let codes = [];
for (const ch of s) codes.push(ch);
__L(4, "rope", s.length, codes.length, codes[0], codes[codes.length - 1], codes.join(""));
let w = "\u{1F600}" + "x";
let parts = [];
for (const ch of w) parts.push(ch.codePointAt(0).toString(16));
__A("d05_forof_misc.js:rope-wide", function () { assert_eq(parts.join(","), "1f600,78", "rope-wide"); });
// rope string sliced then iterated
const rope2 = ("xy" + "zw").repeat(10).slice(3, 25);
const got2 = [];
for (const ch of rope2) got2.push(ch);
__L(6, "rope2", rope2.length, got2.length, got2.join(""));
// for-of over string with combining chars (code-unit iteration)
let acc = "";
for (const ch of "e\u0301x") acc += ch.codePointAt(0).toString(16) + " ";
__A("d05_forof_misc.js:comb", function () { assert_eq(acc.trim(), "65 301 78", "comb"); });

summary("bbreview");
