// slowarr differential battery — prints deterministic facts only.
// ser(): engine-neutral array serialization (length + own names + sorted keys +
// descriptors with attributes) so holes/accessors/attributes are all observable.
"use strict";
let out = [];
function log() { out.push(Array.prototype.slice.call(arguments).join(" ")); }

function descstr(d) {
    if (!d) return "absent";
    let s = "";
    if (d.get) s += "accessor";
    else s += "val:" + String(d.value);
    if (!d.writable) s += "!w";
    if (!d.enumerable) s += "!e";
    if (!d.configurable) s += "!c";
    return s;
}
function ser(a) {
    if (!Array.isArray(a)) return "notarray:" + String(a);
    const names = Object.getOwnPropertyNames(a);
    const keys = Object.keys(a);
    let descs = [];
    for (const k of names) {
        const d = Object.getOwnPropertyDescriptor(a, k);
        descs.push(k + "=" + descstr(d));
    }
    return "len=" + a.length + " names=[" + names.join(",") + "] keys=[" + keys.join(",") + "] descs=[" + descs.join(";") + "]";
}
function val(v) {
    if (typeof v === "object" && v !== null) return Array.isArray(v) ? ser(v) : "obj";
    if (typeof v === "symbol") return "symbol";
    if (typeof v === "function") return "fn";
    if (typeof v === "bigint") return "big:" + v.toString();
    return String(v);
}
function chk(name, fn) {
    try {
        log(name, "=>", val(fn()));
        console.error(name);
    } catch (e) {
        log(name, "=> throw", e && e.constructor ? e.constructor.name : String(e));
        console.error("THREW " + name);
    }
}

// ---------- shapes ----------
const SYM = Symbol("s");
function dense() { let a = []; for (let i = 0; i < 8; i++) a.push(i * 10); return a; }
function denseSlow() { let a = dense(); delete a[3]; a[3] = 30; return a; } // occ==len, slow
function holeyMid() { let a = dense(); delete a[3]; return a; }             // 1 hole at 3
function holeyStart() { let a = dense(); delete a[0]; return a; }
function holeyEnd() { let a = dense(); delete a[7]; return a; }
function verySparse() { let a = []; a[1000000] = 7; return a; }
function allHoles() { let a = new Array(5); return a; }
function withUndef() { let a = dense(); a[3] = undefined; return a; }
function withNeg0() { let a = dense(); a[3] = -0; return a; }
function withNaN() { let a = dense(); a[3] = NaN; return a; }
function withSym() { let a = dense(); a[3] = SYM; a[4] = BigInt(90); return a; }
function frozenArr() { return Object.freeze(holeyMid()); }
function sealedArr() { return Object.seal(holeyMid()); }
function getterArr() {
    let a = dense();
    Object.defineProperty(a, 3, { get() { return 999; }, configurable: true });
    return a;
}
class Sub extends Array {}
function subArr() { let s = new Sub(); for (let i = 0; i < 5; i++) s.push(i * 10); delete s[3]; return s; }
function arrayLike() { return { length: 5, 2: "x" }; }

const shapes = [
    ["dense", dense], ["denseSlow", denseSlow], ["holeyMid", holeyMid],
    ["holeyStart", holeyStart], ["holeyEnd", holeyEnd], ["verySparse", verySparse],
    ["allHoles", allHoles], ["withUndef", withUndef], ["withNeg0", withNeg0],
    ["withNaN", withNaN], ["withSym", withSym], ["frozen", frozenArr],
    ["sealed", sealedArr], ["getter", getterArr], ["sub", subArr],
    ["arrayLike", arrayLike],
];

// ---------- T1: includes / indexOf / lastIndexOf ----------
const needles = [
    ["undefined", undefined], ["nan", NaN], ["neg0", -0], ["pos0", 0],
    ["30", 30], ["300", 300], ["str30", "30"], ["obj", {}],
    ["symSame", SYM], ["big90", BigInt(90)], ["big1", BigInt(1)],
];
for (const [sname, mk] of shapes) {
    for (const [nname, needle] of needles) {
        chk(`inc ${sname} ${nname}`, () => mk().includes(needle));
        chk(`iof ${sname} ${nname}`, () => mk().indexOf(needle));
        chk(`liof ${sname} ${nname}`, () => mk().lastIndexOf(needle));
    }
}

const fromIdx = [
    ["undef", undefined], ["0", 0], ["neg3", -3], ["neg100", -100],
    ["8", 8], ["100", 100], ["half", 0.5], ["nanF", NaN],
    ["i32max", 2147483647], ["i32min", -2147483648], ["str2", "2"],
];
for (const [sname, mk] of [["holeyMid", holeyMid], ["verySparse", verySparse], ["allHoles", allHoles], ["denseSlow", denseSlow]]) {
    for (const [fname, fi] of fromIdx) {
        chk(`inc ${sname} fi=${fname}`, () => mk().includes(30, fi));
        chk(`iof ${sname} fi=${fname}`, () => mk().indexOf(30, fi));
        chk(`liof ${sname} fi=${fname}`, () => mk().lastIndexOf(30, fi));
        chk(`incU ${sname} fi=${fname}`, () => mk().includes(undefined, fi));
        chk(`iofU ${sname} fi=${fname}`, () => mk().indexOf(undefined, fi));
        chk(`liofU ${sname} fi=${fname}`, () => mk().lastIndexOf(undefined, fi));
    }
}

// huge-length edge (int64 fromIndex clamp)
chk("iof hugeLen fi=4294967294", () => { let a = []; a[4294967294] = 1; return a.indexOf(1, 4294967294); });
chk("inc hugeLen fi=4294967290", () => { let a = []; a[4294967294] = 1; return a.includes(1, 4294967290); });
chk("liof hugeLen", () => { let a = []; a[4294967294] = 1; return a.lastIndexOf(1); });

// ---------- T2: prototype interplay (THE guard case) ----------
chk("proto5 iof holey", () => {
    let a = holeyMid();
    Array.prototype[3] = "proto";
    try { return [a.indexOf("proto"), a.includes("proto"), a.lastIndexOf("proto"), a.indexOf(40)]; }
    finally { delete Array.prototype[3]; }
});
chk("proto5 iof shifted", () => {
    let a = holeyMid();
    Array.prototype[3] = "proto";
    try { a.shift(); return ser(a); } finally { delete Array.prototype[3]; }
});
chk("proto5 iof unshifted", () => {
    let a = holeyMid();
    Array.prototype[4] = "proto";
    try { a.unshift("u"); return ser(a); } finally { delete Array.prototype[4]; }
});
chk("objproto5 iof holey", () => {
    let a = holeyMid();
    Object.prototype[3] = "op";
    try { return [a.indexOf("op"), a.includes("op")]; }
    finally { delete Object.prototype[3]; }
});
chk("protoReversed holey", () => {
    let a = holeyMid();
    Array.prototype[4] = "proto";
    try { a.reverse(); return ser(a); } finally { delete Array.prototype[4]; }
});
chk("protoSliced holey", () => {
    let a = holeyMid();
    Array.prototype[4] = "proto";
    try { return ser(a.slice(1, 8)); } finally { delete Array.prototype[4]; }
});
chk("protoConcated holey", () => {
    let a = holeyMid();
    Array.prototype[4] = "proto";
    try { return ser(a.concat([1])); } finally { delete Array.prototype[4]; }
});
chk("protoGainedAfterGuard", () => {
    // species getter that adds an indexed proto prop mid-slice; both engines must agree
    let a = holeyMid();
    let A = Object.create(Array);
    let fired = false;
    Object.defineProperty(a.constructor, Symbol.species, {
        get() {
            if (!fired) { fired = true; Array.prototype[9] = "late"; }
            return Array;
        }, configurable: true,
    });
    try { return ser(a.slice(0)); }
    finally { delete a.constructor[Symbol.species]; delete Array.prototype[9]; }
});

// ---------- T3: unshift / shift / pop / push ----------
for (const [sname, mk] of shapes) {
    chk(`shift ${sname}`, () => { let a = mk(); let r = a.shift(); return [val(r), ser(a)]; });
    chk(`unshift ${sname}`, () => { let a = mk(); a.unshift("u"); return ser(a); });
    chk(`unshift2 ${sname}`, () => { let a = mk(); a.unshift("u1", "u2"); return ser(a); });
    chk(`pop ${sname}`, () => { let a = mk(); let r = a.pop(); return [val(r), ser(a)]; });
}
chk("shift frozen", () => frozenArr().shift());
chk("unshift frozen", () => { let a = frozenArr(); a.unshift(1); return ser(a); });
chk("shift sealed", () => sealedArr().shift());
chk("shift mixedWritability", () => {
    let a = dense(); delete a[3]; a[3] = 30;
    Object.defineProperty(a, 1, { writable: false });
    let r;
    try { r = a.shift(); } catch (e) { return ["throw", e.constructor.name, ser(a)]; }
    return [val(r), ser(a)];
});
chk("shift proxy", () => {
    let a = holeyMid();
    let p = new Proxy(a, {});
    let r = Array.prototype.shift.call(p);
    return [val(r), ser(a)];
});
chk("unshift proxy", () => {
    let a = holeyMid();
    let p = new Proxy(a, {});
    Array.prototype.unshift.call(p, "u");
    return ser(a);
});
chk("shift mutatedDuring", () => {
    // length getter is not spec-observable for shift; use a Proxy get trap on length? not own.
    // instead: species is irrelevant to shift; use Object.defineProperty on length? skip.
    let a = holeyMid();
    Object.defineProperty(a, "length", { value: a.length, writable: false, configurable: true });
    try { return String(a.shift()); } catch (e) { return "throw:" + e.constructor.name; }
});

// ---------- T4: slice ----------
const slices = [[0], [1], [2, 6], [-3], [3], [5, 3], [-100, 100], [2, undefined], [2.5], [-2.5]];
for (const [sname, mk] of shapes) {
    for (const args of slices) {
        chk(`slice ${sname} [${args}]`, () => { let a = mk(); return ser(a.slice(...args)); });
    }
}
chk("slice speciesArr", () => {
    let a = holeyMid();
    Object.defineProperty(a.constructor, Symbol.species, { value: Array, configurable: true });
    try { return ser(a.slice()); } finally { delete a.constructor[Symbol.species]; }
});
chk("slice speciesMutator", () => {
    let a = holeyMid();
    Object.defineProperty(a.constructor, Symbol.species, {
        value(len) { a.length = 2; return new Array(len); }, configurable: true,
    });
    try { return ser(a.slice()); } finally { delete a.constructor[Symbol.species]; }
});
chk("slice speciesThrowGetter", () => {
    let a = holeyMid();
    Object.defineProperty(a.constructor, Symbol.species, {
        get() { throw new TypeError("sp"); }, configurable: true,
    });
    try { return a.slice(); } catch (e) { return "throw:" + e.constructor.name; }
    finally { delete a.constructor[Symbol.species]; }
});
chk("slice subResult", () => ser(subArr().slice(1, 4)));

// ---------- T5: concat ----------
for (const [sname, mk] of shapes) {
    chk(`concat ${sname}`, () => { let a = mk(); return ser(a.concat([1, 2])); });
    chk(`concatArg ${sname}`, () => { let a = mk(); return ser([].concat(a, 5)); });
}
chk("concat subSpecies", () => {
    let a = holeyMid();
    return ser(a.concat(subArr()));
});
chk("concat proxyArg", () => {
    let a = holeyMid();
    let p = new Proxy(a, {});
    let r;
    try { r = [].concat(p); } catch (e) { return "throw:" + e.constructor.name; }
    return ser(r);
});

// ---------- T6: reverse ----------
for (const [sname, mk] of shapes) {
    chk(`reverse ${sname}`, () => { let a = mk(); a.reverse(); return ser(a); });
}
chk("reverse frozen", () => { let a = frozenArr(); a.reverse(); return ser(a); });
chk("reverse sealed", () => { let a = sealedArr(); a.reverse(); return ser(a); });
chk("reverse mixedWritability", () => {
    let a = dense(); delete a[3]; a[3] = 30;
    Object.defineProperty(a, 1, { writable: false });
    try { a.reverse(); return ser(a); } catch (e) { return ["throw", e.constructor.name, ser(a)]; }
});
chk("reverse proxy", () => {
    let a = holeyMid();
    Array.prototype.reverse.call(new Proxy(a, {}));
    return ser(a);
});
chk("reverse double", () => { let a = holeyMid(); a.reverse(); a.reverse(); return ser(a); });

// ---------- T7: misc receivers / global mutant between calls ----------
chk("search on string", () => [Array.prototype.indexOf.call("abc", "b"), Array.prototype.includes.call("abc", "b")]);
chk("shift on arrayLike", () => { let o = arrayLike(); return [val(Array.prototype.shift.call(o)), JSON.stringify(o)]; });
chk("slice on arrayLike", () => ser(Array.prototype.slice.call(arrayLike())));

log("END");
console.log(out.join("\n"));
