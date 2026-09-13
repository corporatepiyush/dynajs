// slowarr MUTATOR differential battery (diff2) — big shapes + edge cases for
// unshift/shift/slice/reverse/concat slow paths. Byte-compare vs node.
// Same engine-neutral serializer as diff1.js.
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
    let descs = [];
    for (const k of names) {
        const d = Object.getOwnPropertyDescriptor(a, k);
        descs.push(k + "=" + descstr(d));
    }
    return "len=" + a.length + " names=[" + names.join(",") + "] descs=[" + descs.join(";") + "]";
}
// checksum serializer for huge occupied counts (keeps output bounded but
// order-sensitive: iterates keys in enumeration order)
function serBig(a) {
    if (!Array.isArray(a)) return "notarray:" + String(a);
    const keys = Object.keys(a);
    let h = 0, n = keys.length;
    for (let i = 0; i < n; i++) {
        const k = keys[i];
        const v = a[k];
        h = (h * 31 + (typeof k === "string" ? k.length : 0)) | 0;
        const vs = String(v);
        for (let j = 0; j < vs.length; j++) h = (h * 31 + vs.charCodeAt(j)) | 0;
    }
    return "len=" + a.length + " nkeys=" + n + " first=" + keys[0] + " last=" + keys[n - 1] + " ck=" + h;
}
function val(v) {
    if (typeof v === "object" && v !== null) return Array.isArray(v) ? ser(v) : "obj";
    return String(v);
}
function chk(name, fn) {
    try {
        log(name, "=>", val(fn()));
    } catch (e) {
        log(name, "=> throw", e && e.constructor ? e.constructor.name : String(e));
    }
    console.log(out[out.length - 1]); // progressive: survive bisection
}
function chkBig(name, fn) {
    try {
        log(name, "=>", serBig(fn()));
    } catch (e) {
        log(name, "=> throw", e && e.constructor ? e.constructor.name : String(e));
    }
    console.log(out[out.length - 1]); // progressive
}

// ---------- big shape builders ----------
function holey1k() { let a = []; for (let i = 0; i < 1000; i++) if (i % 7 !== 3) a[i] = i * 2; return a; } // ~857 occ
function sparse1e6() {
    let a = new Array(1000000);
    for (let i = 0; i < 500; i++) a[i * 1999] = i;
    return a;
}
function denseSlow1k() { let a = holey1k(); a[3] = 6; return a; } // occ==len, still slow
function mixedKeys() { let a = holey1k(); a.foo = "f"; a.bar = "b"; a[999] = 1998; return a; }
function mixedKeysDense() { let a = denseSlow1k(); a.x = "ex"; a[500] = 1000; return a; }
function frozen1k() { return Object.freeze(holey1k()); }
function sealed1k() { return Object.seal(holey1k()); }
class Sub extends Array {}
function sub1k() { let s = new Sub(); for (let i = 0; i < 500; i++) s.push(i); delete s[100]; return s; }
function getterMid1k() {
    let a = holey1k();
    Object.defineProperty(a, 500, { get() { return "acc"; }, configurable: true });
    return a;
}

// ---------- M1: shift/unshift on big shapes ----------
for (const [nm, mk] of [["holey1k", holey1k], ["sparse1e6", sparse1e6],
                        ["denseSlow1k", denseSlow1k], ["mixedKeys", mixedKeys],
                        ["mixedKeysDense", mixedKeysDense], ["sub1k", sub1k],
                        ["getterMid1k", getterMid1k]]) {
    chkBig(`shift ${nm}`, () => { let a = mk(); let r = a.shift(); a._r = r; return a; });
    chkBig(`unshift ${nm}`, () => { let a = mk(); a.unshift("u"); return a; });
    chkBig(`unshift3 ${nm}`, () => { let a = mk(); a.unshift("u1", "u2", "u3"); return a; });
    chkBig(`shiftx3 ${nm}`, () => { let a = mk(); let r = [a.shift(), a.shift(), a.shift()]; a._r = r.join("|"); return a; });
    chkBig(`pair ${nm}`, () => { let a = mk(); a.unshift("u"); a.shift(); return a; });
}
chk("unshift frozen1k", () => { let a = frozen1k(); a.unshift(1); return ser(a).slice(0, 80); });
chk("shift frozen1k", () => { let a = frozen1k(); try { a.shift(); } catch (e) { return "throw:" + e.constructor.name; } return ser(a).slice(0, 80); });
chk("shift sealed1k", () => { let a = sealed1k(); try { a.shift(); } catch (e) { return "throw:" + e.constructor.name; } return "ok len=" + a.length; });

// ---------- M2: reverse on big shapes ----------
for (const [nm, mk] of [["holey1k", holey1k], ["sparse1e6", sparse1e6],
                        ["denseSlow1k", denseSlow1k], ["mixedKeys", mixedKeys],
                        ["sub1k", sub1k], ["getterMid1k", getterMid1k]]) {
    chkBig(`reverse ${nm}`, () => { let a = mk(); a.reverse(); return a; });
    chkBig(`rev2 ${nm}`, () => { let a = mk(); a.reverse(); a.reverse(); return a; });
}
chk("reverse frozen1k", () => { let a = frozen1k(); try { a.reverse(); } catch (e) { return "throw:" + e.constructor.name; } return "ok"; });

// ---------- M3: slice/concat on big shapes ----------
for (const [nm, mk] of [["holey1k", holey1k], ["sparse1e6", sparse1e6],
                        ["mixedKeys", mixedKeys], ["sub1k", sub1k]]) {
    chkBig(`slice ${nm}`, () => mk().slice(10, 900));
    chkBig(`sliceNeg ${nm}`, () => mk().slice(-50));
    chkBig(`sliceAll ${nm}`, () => mk().slice());
    chkBig(`concat ${nm}`, () => mk().concat([1, 2]));
    chkBig(`concatArg ${nm}`, () => [].concat(mk(), "z"));
}
chk("slice frozen1k", () => serBig(frozen1k().slice(5, 20)));

// ---------- M4: proto with indexed props on the mutators ----------
chk("protoShift1k", () => {
    let a = holey1k();
    Array.prototype[4] = "p4";
    try { a.shift(); return serBig(a); } finally { delete Array.prototype[4]; }
});
chk("protoUnshift1k", () => {
    let a = holey1k();
    Array.prototype[2] = "p2";
    try { a.unshift("u"); return serBig(a); } finally { delete Array.prototype[2]; }
});
chk("protoReverse1k", () => {
    let a = holey1k();
    Array.prototype[500] = "p500";
    try { a.reverse(); return serBig(a); } finally { delete Array.prototype[500]; }
});
chk("protoSlice1k", () => {
    let a = holey1k();
    Array.prototype[100] = "p100";
    try { return serBig(a.slice(0, 200)); } finally { delete Array.prototype[100]; }
});
chk("protoConcat1k", () => {
    let a = holey1k();
    Array.prototype[100] = "p100";
    try { return serBig(a.concat([9])); } finally { delete Array.prototype[100]; }
});
chk("objProtoUnshift1k", () => {
    let a = holey1k();
    Object.prototype[7] = "o7";
    try { a.unshift("u"); return serBig(a); } finally { delete Object.prototype[7]; }
});

// ---------- M5: getters on indices / mutation during op ----------
chk("getter0 shift mutates", () => {
    // getter at 0 fires (Get(O,0)) before the move in every engine; it
    // mutates the array mid-operation
    let a = holey1k();
    Object.defineProperty(a, 0, { get() { delete a[10]; a[2000] = "side"; return "g0"; }, configurable: true });
    let r = a.shift();
    return [r, serBig(a)];
});
chk("getter0 shift shrinks", () => {
    let a = holey1k();
    Object.defineProperty(a, 0, { get() { a.length = 3; return "g0"; }, configurable: true });
    let r;
    try { r = a.shift(); } catch (e) { return ["throw:", e.constructor.name, "len=" + a.length]; }
    return [r, serBig(a)];
});
chk("getter0 unshift absent", () => {
    // unshift never reads index 0 generically (moves only); a getter at 0
    // that is ALSO at the move target must not fire in either engine
    let a = holey1k();
    let fired = 0;
    Object.defineProperty(a, 0, { get() { fired++; return "g0"; }, set(v) { fired += 10; }, configurable: true });
    a.unshift("u");
    return ["fired=" + fired, serBig(a)];
});
chk("getterMid splice", () => {
    let a = holey1k();
    Object.defineProperty(a, 400, { get() { a[401] = "side"; return "g400"; }, configurable: true });
    let r = a.splice(399, 3, "s");
    return [serBig(r), serBig(a)];
});
chk("species mutates source slice", () => {
    let a = holey1k();
    Object.defineProperty(a.constructor, Symbol.species, {
        value(len) { a[5] = "mut"; delete a[10]; return new Array(len); }, configurable: true,
    });
    try { return serBig(a.slice()); } finally { delete a.constructor[Symbol.species]; }
});
chk("species proxy result concat", () => {
    let a = holey1k();
    Object.defineProperty(a.constructor, Symbol.species, {
        value() { return new Proxy([], {}); }, configurable: true,
    });
    try { return a.concat([1]).length; } finally { delete a.constructor[Symbol.species]; }
});

// ---------- M6: attribute edges on big shapes ----------
chk("writableOff shift1k", () => {
    let a = holey1k();
    Object.defineProperty(a, 5, { writable: false });
    let r;
    try { r = a.shift(); } catch (e) { return ["throw:", e.constructor.name, serBig(a)]; }
    return [r, serBig(a)];
});
chk("nonconfUnshift1k", () => {
    let a = holey1k();
    Object.defineProperty(a, 2, { configurable: false });
    try { a.unshift("u"); return serBig(a); } catch (e) { return "throw:" + e.constructor.name; }
});
chk("nonconfReverse1k", () => {
    let a = holey1k();
    Object.defineProperty(a, 2, { configurable: false });
    try { a.reverse(); return serBig(a); } catch (e) { return "throw:" + e.constructor.name; }
});

// ---------- M7: repeated ops keep shape stats sane (rehash/compact) ----------
chk("churn holey1k", () => {
    let a = holey1k();
    for (let i = 0; i < 50; i++) { a.unshift(i); a.shift(); }
    for (let i = 0; i < 10; i++) { delete a[i * 13]; a[i * 13] = i; }
    a.reverse();
    return serBig(a);
});
chk("churn sparse1e6", () => {
    let a = sparse1e6();
    for (let i = 0; i < 20; i++) { a.unshift(i); a.shift(); }
    return serBig(a);
});
chk("shift to empty", () => {
    let a = [1];
    delete a[0];
    let r = a.shift();
    return [String(r), ser(a)];
});
chk("unshift allHoles", () => {
    let a = new Array(4);
    a.unshift("u");
    return ser(a);
});
chk("reverse allHoles", () => {
    let a = new Array(4);
    a.reverse();
    return ser(a);
});

// ---------- M8: atom boundary near 2^31 ----------
// Moved to diff2_edge.js: any array-index >= 2^31 forces length >= 2^31+1,
// and node's dictionary-mode mutators are O(len) there (minutes per op, with
// Smi-overflow results that are not spec-derived), so node cannot serve as
// the oracle for this scale. The dyna fast paths either run in O(occupied)
// (tagged indices <= INT32_MAX) or bail to the proven generic path
// (string-atom indices -- proven to bail via lldb_edge_guard.txt).

log("END");
console.log("END");
