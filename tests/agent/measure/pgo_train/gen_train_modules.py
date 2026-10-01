#!/usr/bin/env python3
"""Generate the 24 mod_*.js modules that train_modules.js imports.
Run once from tests/agent/measure/pgo_train/:  python3 gen_train_modules.py
The generated modules are committed so the training set is reproducible
without python. Each module exercises a different engine subsystem so the
PGO profile sees the whole interpreter, not one hot handler."""
import os
HERE = os.path.dirname(os.path.abspath(__file__))

MODS = {
"mod_a.js": """
export function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
export function ackermann(m, n) {
  if (m === 0) return n + 1;
  if (n === 0) return ackermann(m - 1, 1);
  return ackermann(m - 1, ackermann(m, n - 1));
}
""",
"mod_b.js": """
export function lcg(n, seed) {
  const a = new Array(n); let s = seed >>> 0;
  for (let i = 0; i < n; i++) { s = (s * 1103515245 + 12345) >>> 0; a[i] = s % 100003; }
  return a;
}
export function qsort(a) {
  if (a.length < 2) return a;
  const p = a[a.length >> 1], lo = [], eq = [], hi = [];
  for (const x of a) { if (x < p) lo.push(x); else if (x > p) hi.push(x); else eq.push(x); }
  return [...qsort(lo), ...eq, ...qsort(hi)];
}
""",
"mod_c.js": """
export function rot13(s) {
  let out = "";
  for (const ch of s) {
    const c = ch.codePointAt(0);
    if (c >= 65 && c <= 90) out += String.fromCharCode((c - 65 + 13) % 26 + 65);
    else if (c >= 97 && c <= 122) out += String.fromCharCode((c - 97 + 13) % 26 + 97);
    else out += ch;
  }
  return out;
}
export function piglatin(s) {
  return s.split(/([aeiou]\\w*)/).filter(Boolean).join("-") + " ay";
}
""",
"mod_d.js": """
export class Matrix {
  constructor(n) { this.n = n; this.d = new Float64Array(n * n).fill(1.5); }
  trace() { let t = 0; for (let i = 0; i < this.n; i++) t += this.d[i * this.n + i]; return t; }
}
export function matmul(a, b) {
  const n = a.n, c = new Matrix(n), d = c.d, ad = a.d, bd = b.d;
  for (let i = 0; i < n; i++)
    for (let k = 0; k < n; k++) {
      const aik = ad[i * n + k];
      for (let j = 0; j < n; j++) d[i * n + j] += aik * bd[k * n + j];
    }
  return c;
}
""",
"mod_e.js": """
export class LRUCache {
  constructor(cap) { this.cap = cap; this.m = new Map(); }
  get(k) { if (!this.m.has(k)) return undefined; const v = this.m.get(k); this.m.delete(k); this.m.set(k, v); return v; }
  put(k, v) { if (this.m.has(k)) this.m.delete(k); else if (this.m.size >= this.cap) this.m.delete(this.m.keys().next().value); this.m.set(k, v); }
}
""",
"mod_f.js": """
export function parseCsv(text) {
  const lines = text.split("\\n").filter(l => l.length);
  const rows = lines.map(l => l.split(",").map(f => /^\\d+$/.test(f) ? parseInt(f, 10) : f));
  return { header: rows[0], rows: rows.slice(1) };
}
""",
"mod_g.js": """
export function tokenizer(src) {
  let i = 0, count = 0; const toks = [];
  while (i < src.length) {
    const c = src[i];
    if (c === " " || c === "\\n") { i++; continue; }
    if (c >= "0" && c <= "9") { let j = i; while (j < src.length && src[j] >= "0" && src[j] <= "9") j++; toks.push(src.slice(i, j)); i = j; }
    else if (/[a-zA-Z_$]/.test(c)) { let j = i; while (j < src.length && /\\w/.test(src[j])) j++; toks.push(src.slice(i, j)); i = j; }
    else { toks.push(c); i++; }
    count++;
  }
  return { toks, count };
}
""",
"mod_h.js": """
export function bignumMul(a, b) {
  const A = a.split("").reverse().map(Number), B = b.split("").reverse().map(Number);
  const C = new Array(A.length + B.length).fill(0);
  for (let i = 0; i < A.length; i++) {
    let carry = 0;
    for (let j = 0; j < B.length; j++) {
      const cur = C[i + j] + A[i] * B[j] + carry;
      C[i + j] = cur % 10; carry = (cur / 10) | 0;
    }
    C[i + B.length] += carry;
  }
  let s = ""; let k = C.length - 1;
  while (k > 0 && C[k] === 0) k--;
  for (; k >= 0; k--) s += C[k];
  return s;
}
""",
"mod_i.js": """
export function dateFmt(d, n) {
  let sink = 0;
  for (let i = 0; i < n; i++) {
    const t = new Date(d.getTime() + i * 3600e3);
    sink += t.getUTCFullYear() + t.getUTCMonth() + t.getUTCDate() + t.getUTCHours();
  }
  return sink % 7;
}
""",
"mod_j.js": """
export function regexpDrive(text, n) {
  let sink = 0;
  const re1 = /(ab)+/g, re2 = /abc/g, re3 = /\\d+/g;
  for (let i = 0; i < n; i++) {
    let m; re1.lastIndex = 0; while ((m = re1.exec(text))) sink += m[0].length;
    re2.lastIndex = 0; while ((m = re2.exec(text))) sink += 1;
    re3.lastIndex = 0; while ((m = re3.exec(text))) sink += m[0].length;
    sink += text.replace(/ab/g, "AB").length % 3;
    sink += text.split(" ").length % 5;
  }
  return sink % 97;
}
""",
"mod_k.js": """
const base = { describe() { return "base"; }, v: 1 };
const mid = Object.create(base); mid.describe = function () { return "mid:" + this.v; };
const leaf = Object.create(mid);
export function protoChain(n) {
  let sink = 0;
  for (let i = 0; i < n; i++) { leaf.v = i; sink += leaf.describe().length; sink += base.v; }
  return sink % 101;
}
""",
"mod_l.js": """
export function* gen(n) { for (let i = 0; i < n; i++) yield i * 2; }
export function generators(n) {
  let sink = 0;
  for (const x of gen(n)) sink += x;
  for (const x of gen(n / 2)) sink += x % 7;
  return sink % 103;
}
""",
"mod_m.js": """
export function promises() {
  return Promise.all([1, 2, 3, 4, 5].map(i => Promise.resolve(i).then(v => v * 2)))
    .then(rs => rs.reduce((a, b) => a + b, 0));
}
""",
"mod_n.js": """
export function maps(n) {
  const m = new Map(), s = new Set(); let sink = 0;
  for (let i = 0; i < n; i++) { m.set(i, i * 3); s.add(i % (n / 2)); }
  for (const [k, v] of m) sink += (k ^ v) % 11;
  for (const x of s) sink += x % 13;
  return sink % 107;
}
""",
"mod_o.js": """
const k1 = Symbol("k1"), k2 = Symbol.for("shared.k2");
export function symbols(n) {
  const o = { [k1]: 1, [k2]: 2, plain: 3 }; let sink = 0;
  for (let i = 0; i < n; i++) { o[k1] = i; sink += o[k1] + o[k2] + o.plain; }
  return sink % 109;
}
""",
"mod_p.js": """
export class P { constructor(x) { this._x = x; } get x() { return this._x; } set x(v) { this._x = v; } }
export function getters(n) {
  let sink = 0; const p = new P(1);
  for (let i = 0; i < n; i++) { p.x = i; sink += p.x; }
  return sink % 113;
}
""",
"mod_q.js": """
class Animal { constructor(name) { this.name = name; } speak() { return this.name + " makes a noise."; } }
class Dog extends Animal { speak() { return super.speak() + " Woof."; } }
class Puppy extends Dog { speak() { return super.speak() + " Yip!"; } }
export function classes(n) {
  let sink = 0; const d = new Dog("Rex"), p = new Puppy("Bit");
  for (let i = 0; i < n; i++) sink += d.speak().length + p.speak().length + i;
  return sink % 127;
}
""",
"mod_r.js": """
export function typed(n) {
  const f = new Float64Array(n), i32 = new Int32Array(n); let sink = 0;
  for (let i = 0; i < n; i++) f[i] = i * 1.5;
  for (let i = 0; i < n; i++) i32[i] = f[i] | 0;
  for (let i = 0; i < n; i++) sink += i32[i] % 17;
  return sink % 131;
}
""",
"mod_s.js": """
export function jsonDrive(sample, n) {
  let sink = 0;
  for (let i = 0; i < n; i++) {
    const o = JSON.parse(sample);
    o.k[0] = i; o.o.n = i;
    sink += JSON.stringify(o).length % 19;
  }
  return sink % 137;
}
""",
"mod_t.js": """
export function errorPaths(n) {
  let sink = 0, caught = 0, thrown = 0;
  for (let i = 0; i < n; i++) {
    try {
      if (i % 3 === 0) { thrown++; throw new TypeError("t" + i); }
      if (i % 5 === 0) { thrown++; throw new RangeError("r" + i); }
      sink += i;
    } catch (e) { caught++; sink += e.message.length % 7; }
    finally { sink ^= 1; }
  }
  return (sink + caught + thrown) % 139;
}
""",
"mod_u.js": """
export function stringBuild(n) {
  const parts = [];
  for (let i = 0; i < n; i++) parts.push("item" + i + ":" + (i * 31));
  const joined = parts.join(",");
  let sink = 0;
  for (const p of parts) sink += p.length;
  return (joined.length + sink) % 149;
}
""",
"mod_v.js": """
export function sortDrive(n) {
  const a = new Array(n); let s = 12345;
  for (let i = 0; i < n; i++) { s = (s * 48271) % 2147483647; a[i] = s % n; }
  a.sort((x, y) => x - y);
  let sink = 0;
  for (let i = 1; i < n; i++) if (a[i] < a[i - 1]) sink++;
  a.sort();
  return (sink === 0 ? 1 : 0) + (a[0] === undefined ? 1 : 0);
}
""",
"mod_w.js": """
function sumTo(n, acc) { return n <= 0 ? acc : sumTo(n - 1, acc + n); }
function even(n) { return n === 0 ? true : odd(n - 1); }
function odd(n) { return n === 0 ? false : even(n - 1); }
export function tailRec(n) { return sumTo(n, 0) % 151 + (even(n) ? 2 : 1); }
""",
"mod_x.js": """
export function iterators(n) {
  const o = { *[Symbol.iterator]() { for (let i = 0; i < 10; i++) yield i; } };
  let sink = 0;
  for (let r = 0; r < n / 10; r++) for (const x of o) sink += x;
  const arr = Array.from({ length: n / 10 }, (_, i) => i);
  sink += arr.reduce((a, b) => a + b, 0);
  sink += [...arr.slice(0, 50), ...arr.slice(50, 100)].length;
  return sink % 157;
}
""",
}

for name, body in MODS.items():
    with open(os.path.join(HERE, name), "w") as f:
        f.write("// Generated by gen_train_modules.py -- PGO training module\n" + body + "\n")
print(f"wrote {len(MODS)} modules to {HERE}")
