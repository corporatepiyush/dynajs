// covers: c:src/vm/tailcall.inc.c
// Prepared callback frames: C loops that call one bytecode callback many times (Iterator.prototype forEach/some/every/
// find/reduce, the map/filter/flatMap callbacks of a helper chain driven by toArray or those consumers, and
// Array.prototype every/some/forEach/map/filter/reduce/reduceRight/find/findIndex/findLast/findLastIndex) push the callee
// frame once (js_call_prep_begin), re-seed only its arguments, locals and closure slots per call, run it, and keep the
// frame (TC_FR_KEEP) until js_call_prep_end. A kept frame never takes the tail-call frame replacement (its tail calls run as
// ordinary calls), and a helper's prepared frame is detached when the consumer ends. Every expected value below is what
// node 26 prints for the same code; the cases pin what a reused frame must not leak from one call to the next (thrown
// calls, parameters past argc, locals, per-call closures, arguments, defaults), re-entry into the helper being consumed,
// this binding, callbacks that are not plain functions, stack traces, array holes and mutation, early-stopped consumers
// whose helper is used again afterwards, and the vstack staying balanced after many thrown calls.
import { sections } from "../lib/kit.js";
let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }
const S = sections("test_call_prep_frames");
const arr = [1, 2, 3, 4, 5];
const vals = () => arr.values();
const R = (name, f) => S.section(name, () => { let v; try { v = f(); } catch (e) { v = "threw " + e.constructor.name + ": " + e.message; } const got = JSON.stringify(v); ok(got === WANT[name], name + ": got " + got + ", want " + WANT[name]); });
const WANT = {
    "throw then reuse":"[1,\"t2\",1,2,\"t3\",1,2,3,\"t4\"]",
    "closures per call":"[10,21,32,43,54]",
    "closures per call array":"[10,21,32,43,54]",
    "closures reduce":"[1,2,3,4,5]",
    "arguments":"[\"2:1,0\",\"2:2,1\",\"2:3,2\",\"2:4,3\",\"2:5,4\",3,3,3,3,3]",
    "extra params reset":"[\"undefinedundefined\",\"undefinedundefined\",\"undefinedundefined\",\"undefinedundefined\",\"undefinedundefined\"]",
    "extra params reset array":"[\"undefined\",\"undefined\",\"undefined\",\"undefined\",\"undefined\"]",
    "locals reset":"[null,null,null,null,null,null,null,null,null,null]",
    "default params":"[1,3,5,7,9]",
    "tail call in callback":"[2,4,6,8,10,2,4,6,8,10]",
    "deep recursion in callback":"[100,200,300,400,500]",
    "tail recursion in callback":"[100,200,300,400,500]",
    "reenter helper":"[\"TypeError\",\"TypeError\",\"TypeError\",\"TypeError\",\"TypeError\"]",
    "nested consumer same helper":"[2,4,[6,8,10]]",
    "consumer callback pulls helper":"[101,102,103,104,105,null]",
    "sloppy this":"[true,true,true,true,true,true,true,true,true,true]",
    "strict this":"[null,null,null,null,null,\"T\",\"T\",\"T\",\"T\",\"T\"]",
    "thisArg array":"[11,12,13,14,15,21,22,23,24,25]",
    "class ctor callback":"[\"TypeError\",\"TypeError\"]",
    "async and generator callbacks":"[15,0]",
    "stack names":"true",
    "holes and mutation":"[\"0=1\",\"2=3\",\"3=4\",\"s0\",\"s2\",\"s3\"]",
    "reduce no init":"[15,120,\"54321\"]",
    "find variants":"[3,2,3,2,4]",
    "filter map chain":"[3,-3,9,-9,15]",
    "throw in chain closes":"[\"closed\",\"m\"]",
    "callback count":"10",
    "bound and native callbacks":"[[1,2,3,4,5],[11,12,13,14,15],[\"1\",\"2\",\"3\",\"4\",\"5\"]]",
    "helper after early stop":"[true,[],3,{\"done\":true},[]]",
    "reduce over map chain throws":"\"r\"",
    "holes and mutation via call":"[\"0=1\",\"2=3\",\"3=4\",\"4=5\",\"s0=1\",\"s1=2\",11,\"undefined\"]",
    "helper used after early stop and clobber":"[true,60,2,[],3,[]]",
    "throw at vstack chunk boundaries":"[true,1000]",
    "helper with own return survives consumer":"[true,[-93,-92],[6,8,10],30,[40,50],{\"done\":true}]",
    "kept frame throw under live frames":"[[\"c5\",1,697,40,2,40,4],{\"done\":true}]",
    "vstack balance":"true",
};
R("throw then reuse", () => { const r = []; for (let k = 0; k < 3; k++) { try { vals().forEach((x) => { if (x === 2 + k) throw new Error("t" + x); r.push(x); }); } catch (e) { r.push(e.message); } } return r; });
R("closures per call", () => { const fs = []; vals().forEach((x, i) => { fs.push(() => x * 10 + i); }); return fs.map((f) => f()); });
R("closures per call array", () => { const fs = []; arr.forEach((x, i) => { fs.push(() => x * 10 + i); }); return fs.map((f) => f()); });
R("closures reduce", () => vals().reduce((acc, x) => { acc.push(() => x); return acc; }, []).map((f) => f()));
R("arguments", () => { const r = []; vals().forEach(function () { r.push(arguments.length + ":" + Array.from(arguments).join(",")); }); arr.some(function () { r.push(arguments.length); return false; }); return r; });
R("extra params reset", () => { const r = []; vals().forEach((x, i, extra, more) => { r.push(String(extra) + String(more)); extra = 7; more = 8; }); return r; });
R("extra params reset array", () => { const r = []; arr.forEach((x, i, a, extra) => { r.push(String(extra)); extra = 9; }); return r; });
R("locals reset", () => { const r = []; vals().forEach((x) => { let y; r.push(y); y = x; var z; r.push(z); z = x; }); return r; });
R("default params", () => { const r = []; vals().forEach((x, i = 100, j = i + 1) => { r.push(i + j); }); return r; });
R("tail call in callback", () => { function g(x) { return x * 2; } return vals().map((x) => g(x)).toArray().concat(arr.map((x) => g(x))); });
R("deep recursion in callback", () => { function d(n) { return n === 0 ? 0 : 1 + d(n - 1); } return vals().map((x) => d(x * 100)).toArray(); });
R("tail recursion in callback", () => { "use strict"; function t(n, a) { return n === 0 ? a : t(n - 1, a + 1); } return vals().map((x) => t(x * 100, 0)).toArray(); });
R("reenter helper", () => { const it = vals().map((x) => { try { it.next(); return "no"; } catch (e) { return e.constructor.name; } }); return it.toArray(); });
R("nested consumer same helper", () => { const it = vals().map((x) => x * 2); const r = []; it.forEach((x) => { r.push(x); if (x === 4) r.push(it.toArray()); }); return r; });
R("consumer callback pulls helper", () => { const it = vals().map((x) => x + 100); const r = []; it.forEach((x) => { r.push(x, it.next().value); }); return r; });
R("sloppy this", () => { const r = []; const mk = new Function("r", "return function () { r.push(this === globalThis); }"); vals().forEach(mk(r)); arr.forEach(mk(r)); return r; });
R("strict this", () => { "use strict"; const r = []; vals().forEach(function () { r.push(this); }); arr.forEach(function () { r.push(this); }, "T"); return r; });
R("thisArg array", () => { const r = []; arr.some(function (x) { r.push(this.k + x); return false; }, { k: 10 }); arr.find(function (x) { r.push(this.k + x); }, { k: 20 }); return r; });
R("class ctor callback", () => { class C {} const r = []; try { vals().forEach(C); } catch (e) { r.push(e.constructor.name); } try { arr.forEach(C); } catch (e) { r.push(e.constructor.name); } return r; });
R("async and generator callbacks", () => { let n = 0; vals().forEach(async (x) => { n += x; }); const g = []; arr.forEach(function* () { g.push(1); }); return [n, g.length]; });
R("stack names", () => { function named(x) { if (x === 3) throw new Error("s"); } try { vals().forEach(named); } catch (e) { return /named/.test(e.stack); } });
R("holes and mutation", () => { const a = [1, , 3, 4, 5]; const r = []; a.forEach((x, i) => { r.push(i + "=" + x); if (i === 2) a.length = 4; }); a.some((x, i) => { r.push("s" + i); a.push(9); return i > 6; }); return r; });
R("reduce no init", () => [arr.reduce((a, b) => a + b), vals().reduce((a, b) => a * b), arr.reduceRight((a, b) => a + "" + b)]);
R("find variants", () => [arr.find((x) => x > 2), arr.findIndex((x) => x > 2), arr.findLast((x) => x < 4), arr.findLastIndex((x) => x < 4), vals().find((x) => x === 4)]);
R("filter map chain", () => vals().map((x) => x * 3).filter((x) => x % 2).flatMap((x) => [x, -x]).take(5).toArray());
R("throw in chain closes", () => { const log = []; const src = { i: 0, next() { return { value: this.i++, done: this.i > 5 }; }, return() { log.push("closed"); return {}; }, __proto__: Iterator.prototype }; try { src.map((x) => { if (x === 2) throw new Error("m"); return x; }).toArray(); } catch (e) { log.push(e.message); } return log; });
R("callback count", () => { let calls = 0; vals().filter(() => { calls++; return true; }).map(() => { calls++; return 1; }).toArray(); return calls; });
R("bound and native callbacks", () => [vals().map(Math.abs).toArray(), vals().map(((x, y) => x + y).bind(null, 10)).toArray(), arr.map(String)]);
R("helper after early stop", () => { const it = vals().map((x) => x * 2); const a = it.some((x) => x === 4); const b = it.toArray(); const it2 = vals().filter((x) => x > 1); const c = it2.find((x) => x === 3); return [a, b, c, it2.next(), [...it2]]; });
R("reduce over map chain throws", () => { try { return vals().map((x) => { if (x === 4) throw new TypeError("r"); return x; }).reduce((a, b) => a + b); } catch (e) { return e.message; } });
R("holes and mutation via call", () => { const a = [1, , 3, 4, 5]; const r = []; Array.prototype.forEach.call(a, (x, i) => { r.push(i + "=" + x); if (i === 1) a.length = 3; }); const b = [1, 2, 3, 4, 5, 6]; Array.prototype.some.call(b, (x, i) => { r.push("s" + i + "=" + x); if (i === 0) b.length = 2; return false; }); const c = [5, 6, 7, 8]; r.push(Array.prototype.reduce.call(c, (acc, x, i) => { if (i === 1) c.length = 2; return acc + x; })); r.push(String(Array.prototype.find.call([1, 2, 3, 4], function (x, i, o) { if (i === 0) o.length = 1; return x === undefined; }))); return r; });
R("helper used after early stop and clobber", () => { const it = vals().map((x) => x * 2); const a = it.some((x) => x === 4); function clobber(n) { return n ? [n].map((y) => y + 1).length + clobber(n - 1) : 0; } const c = clobber(60); const d = vals().filter((x) => x > 9).toArray().length + [7, 8].values().map((z) => z).toArray().length; const b = it.toArray(); const f = vals().filter((x) => x % 2); const e = f.find((x) => x === 3); clobber(40); return [a, c, d, b, e, [...f]]; });
R("throw at vstack chunk boundaries", () => { let locals = ""; for (let i = 0; i < 300; i++) locals += "let v" + i + " = x + " + i + ";"; const big = new Function("x", locals + "if (x === 3) throw 1; return v0 + v299;"); let ok = 0, n = 0; function rec(d) { if (d > 0) return rec(d - 1) + 0; try { vals().forEach(big); } catch { ok++; } try { vals().map(big).toArray(); } catch { ok++; } try { Array.prototype.forEach.call(arr, big); } catch { ok++; } return 0; } for (let depth = 0; depth < 1400; depth++) { rec(depth); n++; } function d(m) { return m === 0 ? 0 : 1 + d(m - 1); } return [ok === n * 3, d(1000)]; });
R("helper with own return survives consumer", () => { const it = vals().map((x) => x * 2); it.return = function () { return {}; }; const a = it.some((x) => x === 4); function clobber(n) { return n ? [n].map((y) => y + 1).length + clobber(n - 1) : 0; } clobber(60); const z = [7, 8].values().map((q) => q - 100).toArray(); const b = it.toArray(); const m = vals().filter((x) => x !== 2); const t = m.map((x) => x * 10); t.return = function () { return {}; }; const c = t.find((x) => x === 30); clobber(30); [1, 2].values().filter((q) => q).toArray(); return [a, z, b, c, t.toArray(), m.next()]; });
R("kept frame throw under live frames", () => { let locals = ""; for (let i = 0; i < 300; i++) locals += "let v" + i + " = x + " + i + ";"; const big = new Function("x", locals + "return v0 + v299;"); const r = []; const it = vals().map((x) => { if (x === 2) throw 5; return x; }); function g() { let a = 11, b = 22; try { it.next(); } catch (e) { r.push("c" + e); } big(1); const q = [a, b].map((y) => big(y)); return a + b + q[0] + q[1]; } it.forEach((x) => { r.push(x, g()); }); const s = vals().filter((x) => x > 1); function h() { let k = 40; try { s.next(); } catch (e) { r.push(e); } big(2); return k; } const t = s.map((x) => { if (x === 3) throw "m"; return x; }); try { t.forEach((x) => { r.push(h(), x); }); } catch (e) { r.push("o" + e); } return [r, it.next()]; });
R("vstack balance", () => { function d(n) { return n === 0 ? 0 : 1 + d(n - 1); } let deepest = 0; for (let k = 0; k < 50; k++) { try { vals().forEach((x) => { if (x === 3) throw 1; }); } catch {} try { vals().map((x) => { if (x === 2) throw 1; }).toArray(); } catch {} } let n = 1000; try { for (;; n += 1000) d(n); } catch { deepest = n; } return deepest > 2000; });
S.end();
print(fail ? "FAIL " + fail + " of " + (pass + fail) : "PASS " + pass);
