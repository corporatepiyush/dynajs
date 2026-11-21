// covers: c:src/vm/tailcall.inc.c
// Exception parity for the tail-call interpreter (CONFIG_TAILCALL_INTERP=y, src/vm/tailcall.inc.c). Each loop below runs long enough to execute in
// threaded handlers and then throws from inside one: from the handler's own
// slow path (Symbol/BigInt operands, null base, TDZ reads and writes, derived
// constructor this checks, missing globals) or from code it calls (getter,
// setter, valueOf, a callee of call/call_method/new/spread). Every stack
// frame's line:column and the message must equal what the computed-goto
// interpreter reports; the expected list at the bottom was produced by the
// computed-goto build and must hold in both builds.
// "catch" throws from call, get_field and put_field handlers with operands
// still on the stack and catches in the same frame (plain catch, finally, and
// inside for-of so the iterator is unwound), then keeps looping. "gen" and
// "async" run handler loops between yield/await points (yield and await stay
// on the computed-goto path) with throw()/return() into the generator and a
// rejected await caught in place. "vals"/"shuffles"/"negzero"/"keys"/"proxy"
// pin handler results (-0, unsigned shifts, negative modulo, destructuring
// into member and super targets, one ToPropertyKey per o[k] op=, the
// receiver passed through an exotic prototype).
// A handler that forgets to publish its pc (sf->cur_pc) before calling out,
// exits with the wrong pc or stack, passes the wrong flag/ctx/receiver, or
// permutes the stack wrongly changes one of these lines: proven by planting
// each such fault (45 mutants, each red). Planted faults that move a throw pc
// within one statement (e.g. "'this' can be initialized only once") are not
// observable here or anywhere else and are not claimed.
const out = [];
function where(e) {
    const m = [...e.stack.matchAll(/test_tailcall_parity\.js:(\d+):(\d+)/g)];
    return m.length ? m.map((x) => x[1] + ":" + x[2]).join(",") : "none";
}
function addThrow(n) {
    let s = 0;
    for (let i = 0; i < n; i++) {
        s = s + i;
        if (i === n - 1) s = s + Symbol.iterator;
    }
    return s;
}
function addBig(n) {
    let s = 0;
    for (let i = 0; i < n; i++) {
        s = s + (i === n - 1 ? 10n : i);
    }
    return s;
}
function arrNull(n) {
    let a = [1, 2, 3], t = 0;
    for (let i = 0; i < n; i++) {
        if (i === n - 1) a = null;
        t = t + a[i & 1];
    }
    return t;
}
function ltThrow(n) {
    let x = 0, lim = n;
    for (let i = 0; i < lim; i++) {
        x = x + i;
        if (i === n - 2) lim = Symbol();
    }
    return x;
}
function getterThrow(n) {
    const a = [1, 2];
    Object.defineProperty(Array.prototype, 5, { get() { throw new Error("g"); }, configurable: true });
    let t = 0;
    try {
        for (let i = 0; i < n; i++) t = t + a[i < n - 1 ? 0 : 5];
    } finally {
        delete Array.prototype[5];
    }
    return t;
}
function addValueOf(n) {
    const o = { valueOf() { throw new RangeError("v"); } };
    let s = 0;
    for (let i = 0; i < n; i++) {
        s = s + (i === n - 1 ? o : i);
    }
    return s;
}
function ltValueOf(n) {
    const o = { valueOf() { throw new RangeError("w"); } };
    let lim = n;
    for (let i = 0; i < lim; i++) {
        if (i === n - 2) lim = o;
    }
    return lim;
}
function subSym(n) {
    let s = 0, x = 1;
    for (let i = 0; i < n; i++) {
        if (i === n - 1) x = Symbol();
        s = s - x;
    }
    return s;
}
function mulValueOf(n) {
    const o = { valueOf() { throw new RangeError("m"); } };
    let s = 1;
    for (let i = 0; i < n; i++) s = (s * (i === n - 1 ? o : 1)) % 7;
    return s;
}
function negSym(n) {
    let x = 1, t = 0;
    for (let i = 0; i < n; i++) {
        if (i === n - 1) x = Symbol();
        t = -x;
    }
    return t;
}
function postIncSym(n) {
    let x = 0, t = 0;
    for (let i = 0; i < n; i++) {
        if (i === n - 1) x = Symbol();
        t = x++;
    }
    return t;
}
function decLocValueOf(n) {
    let x = 0;
    for (let i = 0; i < n; i++) {
        if (i === n - 1) x = { valueOf() { throw new RangeError("d"); } };
        x--;
    }
    return x;
}
function notSym(n) {
    let x = 1, t = 0;
    for (let i = 0; i < n; i++) {
        if (i === n - 1) x = Symbol();
        t = ~x;
    }
    return t;
}
function shrValueOf(n) {
    let x = 1, t = 0;
    for (let i = 0; i < n; i++) {
        if (i === n - 1) x = { valueOf() { throw new RangeError("s"); } };
        t = (t ^ (i << 1)) + (i >>> x);
    }
    return t;
}
function cmpValueOf(n) {
    let x = 5, t = 0;
    for (let i = 0; i < n; i++) {
        if (i === n - 1) x = { valueOf() { throw new RangeError("c"); } };
        if (i >= x) t++;
    }
    return t;
}
function eqValueOf(n) {
    let x = 5, t = 0;
    for (let i = 0; i < n; i++) {
        if (i === n - 1) x = { valueOf() { throw new RangeError("e"); } };
        t = t + (i == x);
    }
    return t;
}
function addLocValueOf(n) {
    let s = 0, x = 1;
    for (let i = 0; i < n; i++) {
        if (i === n - 1) x = { valueOf() { throw new RangeError("a"); } };
        s += x;
    }
    return s;
}
function tdzLocal(n) {
    let t = 0;
    for (let i = 0; i < n; i++) {
        if (i === n - 1) t = t + later;
        t = t + 1;
    }
    let later = 1;
    return t + later;
}
function tdzRef(n) {
    const g = () => late;
    let t = 0;
    for (let i = 0; i < n; i++) {
        if (i === n - 1) t = t + g();
        t = t + 1;
    }
    let late = 2;
    return t + late;
}
function postIncValueOf(n) {
    let x = 0, t = 0;
    for (let i = 0; i < n; i++) {
        if (i === n - 1) x = { valueOf() { throw new RangeError("p"); } };
        t = x++;
    }
    return t;
}
function relValueOf(n) {
    let x = 5, t = 0;
    for (let i = 0; i < n; i++) {
        if (i === n - 1) x = { valueOf() { throw new RangeError("r"); } };
        t = t + (i < x);
    }
    return t;
}
class TBase {}
class ThisEarly extends TBase {
    constructor(n) {
        let t = 0;
        for (let i = 0; i < n; i++) t = t + i;
        this.t = t;
        super();
    }
}
class SuperTwice extends TBase {
    constructor(n) {
        let t = 0;
        for (let i = 0; i < n; i++) t = t + i;
        super();
        super();
    }
}
function shuffles(n) {
    const o = {}, q = [0, 0], k = "c", j = 1, src = { a: 1, b: 2, c: 3 };
    let acc = "";
    for (let i = 0; i < n; i++) {
        ({ a: o.x, b: q[j], [k]: o.y } = src);
        ({ [k]: q[0], a: q[j - 1 + 1] } = src);
        acc = "" + o.x + q[1] + o.y + q[0];
    }
    class S { m() { ({ a: super.x } = src); ({ [k]: super[k] } = src); return this.x + "" + this.c; } }
    const it = { [Symbol.iterator]() { return { next: () => ({ value: 1, done: false }), return() { acc += "r"; return {}; } }; } };
    for (const v of it) { if (v) break; }
    return acc + new S().m();
}
function negZero(z, b) {
    let r = 1;
    for (let i = 0; i < 3; i++) r = z * b;
    return Object.is(r, -0) ? "-0" : String(r);
}
class NoSuper extends TBase {
    constructor(n) {
        let t = 0;
        for (let i = 0; i < n; i++) t = t + i;
    }
}
function noSuper(n) { return new NoSuper(n); }
function thrower(i) {
    if (i % 7 === 3) throw new TypeError("t" + i);
    return i;
}
function callThrow(n) {
    let s = 0;
    for (let i = 0; i < n; i++) s = s + thrower(i === n - 1 ? 3 : 1);
    return s;
}
function methodThrow(n) {
    const o = { m(i) { return thrower(i); } };
    let s = 0;
    for (let i = 0; i < n; i++) s = s + o.m(i === n - 1 ? 3 : 1);
    return s;
}
function ctorThrow(n) {
    class C { constructor(i) { this.v = thrower(i); } }
    let s = 0;
    for (let i = 0; i < n; i++) s = s + new C(i === n - 1 ? 3 : 1).v;
    return s;
}
function spreadThrow(n) {
    const args = [1];
    let s = 0;
    for (let i = 0; i < n; i++) {
        if (i === n - 1) args[0] = 3;
        s = s + thrower(...args);
    }
    return s;
}
function fieldGetterThrow(n) {
    const o = { a: 1, get b() { throw new RangeError("fb"); } };
    let s = 0;
    for (let i = 0; i < n; i++) s = s + (i === n - 1 ? o.b : o.a);
    return s;
}
function fieldSetterThrow(n) {
    const o = { a: 1, set b(v) { throw new RangeError("fs"); } };
    for (let i = 0; i < n; i++) {
        if (i === n - 1) o.b = i;
        else o.a = i;
    }
    return o.a;
}
function elSetterThrow(n) {
    const a = [0, 0];
    Object.defineProperty(a, 5, { set(v) { throw new RangeError("es"); } });
    for (let i = 0; i < n; i++) a[i === n - 1 ? 5 : i & 1] = i;
    return a[0];
}
function elIncGetterThrow(n) {
    const a = [0, 0];
    Object.defineProperty(a, 7, { get() { throw new RangeError("ei"); } });
    for (let i = 0; i < n; i++) a[i === n - 1 ? 7 : i & 1] += 1;
    return a[0];
}
function lengthNull(n) {
    let a = "abc", t = 0;
    for (let i = 0; i < n; i++) {
        if (i === n - 1) a = null;
        t = t + a.length;
    }
    return t;
}
function globalMissing(n) {
    let t = 0;
    for (let i = 0; i < n; i++) t = t + (i === n - 1 ? notDeclaredAnywhere : 1);
    return t;
}
function strictGlobalWrite(n) {
    "use strict";
    for (let i = 0; i < n; i++) if (i === n - 1) alsoNotDeclared = i;
    return n;
}
function catchSameFrame(n) {
    const log = [];
    let s = 0, caught = 0;
    const o = { a: 2, get b() { throw new RangeError("cb"); } };
    for (let i = 0; i < n; i++) {
        try {
            s = s + 1 + (i % 50 === 49 ? thrower(3) : 0) * 2;
        } catch (e) {
            caught++;
            if (caught < 3) log.push(where(e));
        }
        try {
            s = s + (i % 40 === 39 ? o.b : o.a);
        } catch (e) {
            caught++;
        } finally {
            s = s + 1;
        }
        try {
            for (const x of [1, 2, 3]) {
                if (i % 60 === 59 && x === 2) null.f = x;
                s = s + x;
            }
        } catch (e) {
            caught++;
        }
    }
    return s + "/" + caught + "/" + log.join(";");
}
function* genLoop(n) {
    let s = 0;
    for (let i = 0; i < n; i++) {
        s = s + i * 2;
        if (i % 100 === 99) {
            try {
                const r = yield s;
                s = s + (r | 0);
            } catch (e) {
                s = s + 1000;
            } finally {
                s = s + 1;
            }
        }
    }
    return s;
}
function genDrive() {
    const g = genLoop(1000), out = [];
    let r = g.next();
    let k = 0;
    while (!r.done) {
        out.push(r.value);
        k++;
        r = k % 3 === 0 ? g.throw(new Error("x")) : g.next(k);
    }
    out.push("done " + r.value);
    const g2 = genLoop(1000);
    g2.next();
    out.push(JSON.stringify(g2.return(7)));
    return out.join(",");
}
async function asyncLoop(n) {
    let s = 0;
    for (let i = 0; i < n; i++) {
        s = s + i;
        if (i % 250 === 249) {
            try {
                s = s + await (i % 500 === 499 ? Promise.reject(new Error("r")) : Promise.resolve(3));
            } catch (e) {
                s = s - 1;
            }
        }
    }
    return s;
}
function proxyReceiver(n) {
    const base = {};
    const o = Object.create(new Proxy(base, { get(t, k, r) { return r === o ? 1 : 100; } }));
    let s = 0;
    for (let i = 0; i < n; i++) s = s + o.zz;
    return s;
}
function keyObjCount(n) {
    let calls = 0;
    const key = { toString() { calls++; return "k"; } };
    const o = { k: 0 };
    for (let i = 0; i < n; i++) o[key] += 1;
    return o.k + "/" + calls;
}
function methodKeyThrow(n) {
    let k = "m" + n, out = "";
    const o = { m200: () => 1 };
    Object.defineProperty(o, "x" + n, { get() { throw new RangeError("mk"); } });
    for (let i = 0; i < n; i++) {
        try { out = o[i === n - 1 ? "x" + n : k]() + k; } catch (e) { out = out + e.message + k.length + k; }
    }
    return out;
}
function thisEarly(n) { return new ThisEarly(n); }
function superTwice(n) { return new SuperTwice(n); }
for (const f of [addThrow, addBig, arrNull, ltThrow, getterThrow, addValueOf, ltValueOf, subSym, mulValueOf, negSym,
    postIncSym, decLocValueOf, notSym, shrValueOf, cmpValueOf, eqValueOf, addLocValueOf, tdzLocal, tdzRef, thisEarly,
    superTwice, postIncValueOf, relValueOf, noSuper, callThrow, methodThrow, ctorThrow, spreadThrow, fieldGetterThrow,
    fieldSetterThrow, elSetterThrow, elIncGetterThrow, lengthNull, globalMissing, strictGlobalWrite]) {
    try {
        f(2000);
        out.push(f.name + " no-throw");
    } catch (e) {
        out.push(f.name + " " + e.constructor.name + " " + where(e) + " " + e.message);
    }
}
const vals = [];
{
    let a = 7, b = -3, f = 2.5, z = 0, m = -2147483648;
    vals.push(a - b, a * b, a / b, a % b, -a % 3, f % 2, 2 ** 10, f ** 2, -z, z * b, -6 % 3, a % -7, m - 1, -m, +true, +null);
    vals.push(a << 30, b >> 1, b >>> 28, a & b, a | b, a ^ b, ~b, f << 1, !a, !z, !"", !"x");
    vals.push(a < f, a <= 7, b > f, a >= 8.5, NaN < 1, a == "7", null == undefined, a === 7.0, a !== 7, a !== 8, a === 8, "x" !== "x", {} == null);
    let p = 1, q = 2, r = 3;
    [p, q, r] = [r, p, q];
    vals.push(p, q, r, a++, a, --b, b);
    let u = 0;
    for (let i = 0; i < 300; i++) { u += i % 3 === 0 ? 1.5 : -1; if (u > 1e9) break; }
    vals.push(u, typeof zzUndeclared, "abcd".length);
}
out.push("vals " + vals.map((v) => Object.is(v, -0) ? "-0" : String(v)).join(","));
out.push("catch " + catchSameFrame(2000));
out.push("gen " + genDrive());
out.push("proxy " + proxyReceiver(500));
out.push("keys " + keyObjCount(50) + " " + methodKeyThrow(200));
out.push("shuffles " + shuffles(3) + " negzero " + negZero(0, -3) + negZero(-2, 0) + negZero(3, 0));
let s = 0;
for (let i = 0; i < 100; i++) s = s + i * 0.5;
out.push("float " + s);
let st = "";
for (let i = 0; i < 5; i++) st = st + i;
out.push("concat " + st);
let big = 2147483600;
for (let i = 0; i < 100; i++) big = big + 1;
out.push("overflow " + big);
function taLen(a, n) {
    let t = 0;
    for (let i = 0; i < n; i++) t += a.length;
    return t;
}
{
    const taP = Object.getPrototypeOf(Float64Array.prototype), d = Object.getOwnPropertyDescriptor(taP, "length");
    const r = [taLen(new Float64Array(3), 50), taLen(new BigInt64Array(2), 50)];
    Object.defineProperty(taP, "length", { get() { return -1; }, configurable: true });
    r.push(taLen(new Uint8Array(9), 50));
    Object.defineProperty(taP, "length", d);
    r.push(taLen(new Uint8Array(9), 50));
    out.push("talen " + r.join(","));
}
function check() {
    const got = out.join("\n");
    const want = "addThrow TypeError 34:32,422:10 cannot convert symbol to number\naddBig TypeError 41:15,422:10 cannot convert bigint to number\narrNull TypeError 49:18,422:10 cannot read property of null\nltThrow TypeError 55:23,422:10 cannot convert symbol to number\ngetterThrow Error 63:72,66:46,422:10 g\naddValueOf RangeError 73:49,76:15,422:10 v\nltValueOf RangeError 81:49,83:23,422:10 w\nsubSym TypeError 92:15,422:10 cannot convert symbol to number\nmulValueOf RangeError 97:49,99:40,422:10 m\nnegSym TypeError 106:13,422:10 cannot convert symbol to number\npostIncSym TypeError 114:14,422:10 cannot convert symbol to number\ndecLocValueOf RangeError 121:64,122:10,422:10 d\nnotSym TypeError 130:13,422:10 cannot convert symbol to number\nshrValueOf RangeError 137:64,138:33,422:10 s\ncmpValueOf RangeError 145:64,146:15,422:10 c\neqValueOf RangeError 153:64,154:20,422:10 e\naddLocValueOf RangeError 161:64,162:11,422:10 a\ntdzLocal ReferenceError 169:32,422:10 later is not initialized\ntdzRef ReferenceError 176:21,179:35,422:10 late is not initialized\nthisEarly ReferenceError 206:9,415:45,422:10 this is not initialized\nsuperTwice ReferenceError 215:14,416:47,422:10 'this' can be initialized only once\npostIncValueOf RangeError 188:64,189:14,422:10 p\nrelValueOf RangeError 196:64,197:20,422:10 r\nnoSuper ReferenceError 239:33,242:41,422:10 this is not initialized\ncallThrow TypeError 244:41,249:48,422:10 t3\nmethodThrow TypeError 244:41,253:24,255:44,422:10 t3\nctorThrow TypeError 244:41,259:48,261:46,422:10 t3\nspreadThrow TypeError 244:41,269:24,422:10 t3\nfieldGetterThrow RangeError 274:53,276:57,422:10 fb\nfieldSetterThrow RangeError 280:54,282:32,422:10 fs\nelSetterThrow RangeError 289:64,290:62,422:10 es\nelIncGetterThrow RangeError 295:63,296:34,422:10 ei\nlengthNull TypeError 303:18,422:10 cannot read property 'length' of null\nglobalMissing ReferenceError 309:56,422:10 'notDeclaredAnywhere' is not defined\nstrictGlobalWrite ReferenceError 314:68,422:10 'alsoNotDeclared' is not defined\nvals 10,-21,-2.3333333333333335,1,-1,0.5,1024,6.25,-0,-0,-0,0,-2147483649,2147483648,1,0,-1073741824,-2,15,5,-1,-6,2,4,false,true,true,false,false,true,false,false,false,true,true,true,false,true,false,false,false,3,1,2,7,8,-4,-4,-50,undefined,4\ncatch 19695/123/244:41,323:49,442:35\ngen 9900,39802,89705,160606,250511,360417,491318,641226,811135,1002036,done 1002047,{\"value\":7,\"done\":true}\nproxy 500\nkeys 50/50 1m200mk4m200\nshuffles 1133r13 negzero -0-00\nfloat 2475\nconcat 01234\noverflow 2147483700\ntalen 150,100,-50,450\nasync 499504";
    if (got !== want) {
        console.log("FAIL got:\n" + got + "\nwant:\n" + want);
        throw new Error("tail-call exception parity");
    }
    console.log("ok test_tailcall_parity");
}
asyncLoop(1000).then((v) => {
    out.push("async " + v);
    check();
}, (e) => {
    console.log("FAIL async " + e);
    throw e;
});
