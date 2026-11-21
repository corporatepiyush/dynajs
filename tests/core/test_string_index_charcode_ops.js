// covers: integration c:src/vm/tailcall.inc.c c:src/vm/interpreter.inc.c c:src/object/property_get.inc.c c:src/runtime/class.inc.c -- s[i], one-unit string equality and charCodeAt share one loop shape across both interpreters
// B8/O12: s[i] and s.charCodeAt(i) in a loop. Three interpreter-level shortcuts, all unobservable:
//  - get_array_el on a string (flat or rope) with an int index inside it reads the code unit in the
//    opcode's slow continuation instead of calling JS_GetPropertyValue; an unflattened rope is read
//    in place (flattening on every read would make s += x; s[s.length-1]
//    quadratic). A rope counts its index reads (16 spare bits in the node) and flattens in place after
//    max(32, len / 256) of them, so a scan over one rope pays the copy once (rope s[i] 62 -> 14 ns)
//    while a rope peeked once or twice (accumulate and peek) never flattens.
//    Out-of-range, negative and non-int keys still take the generic path (prototype).
//  - strict/loose equality of two strings compares pointer, length, then a single unit inline before
//    the general compare (s[i] === '"' compares two one-unit strings).
//  - obj.charCodeAt(i) through the fused method-call ops computes the unit inline when the method
//    found by the ordinary lookup IS the builtin and this is a flat (or flattened rope) string and i
//    an int; anything else (override, getter, String object, rope, double/string index) calls it.
// Flat s[i] 18.6 -> 13.4 ns/iter, charCodeAt 17.9 -> 14.3 (2.1M-char loop; Python 15).
// Oracle: strings built from known code-unit arrays; every case runs inside a loop so the fused ops are
// the ones exercised. Run under both interpreters (the tailcall build and -UCONFIG_TAILCALL_INTERP).
import { suite } from "../lib/kit.js";

const t = suite("string index and charCodeAt fast paths");
const units = [0x61, 0x22, 0x27, 0xe9, 0xff, 0x100, 0x3b1, 0xd800, 0xdc00, 0xffff, 0x30, 0x20];
const flat = String.fromCharCode(...units);
const narrowUnits = units.filter((u) => u < 0x100);
const narrow = String.fromCharCode(...narrowUnits);
function makeRope(parts) {
    let s = "";
    for (const p of parts) s += p;
    return s;
}
function ropeOf(us) {
    return makeRope(us.map((u) => "x".repeat(600) + String.fromCharCode(u)));
}
const ropeUnits = (us) => us.flatMap((u) => [...Array(600).fill(0x78), u]);
function idxLoop(s, n) {
    const out = [];
    for (let i = -2; i < n + 2; i++) out.push(s[i]);
    return out;
}
function ccaLoop(s, n) {
    const out = [];
    for (let i = -2; i < n + 2; i++) out.push(s.charCodeAt(i));
    return out;
}
const want = (us) => [undefined, undefined, ...us.map((u) => String.fromCharCode(u)), undefined, undefined];
const wantCode = (us) => [NaN, NaN, ...us, NaN, NaN];

t.test("s[i] on flat, narrow and rope strings", (a) => {
    a.deep(idxLoop(flat, units.length), want(units), "flat wide");
    a.deep(idxLoop(narrow, narrowUnits.length), want(narrowUnits), "flat narrow");
    const ru = units.concat(units, units), r = ropeOf(ru);
    a.deep(idxLoop(r, r.length), want(ropeUnits(ru)), "rope");
    let acc = "";
    const peeks = [];
    const blk = "y".repeat(700);
    for (let k = 0; k < 300; k++) { acc += blk + String.fromCharCode(0x41 + (k % 26)); peeks.push(acc[acc.length - 1], acc[acc.length - 2]); }
    a.eq(peeks.join(""), Array.from({ length: 300 }, (_, k) => String.fromCharCode(0x41 + (k % 26)) + "y").join(""), "accumulate and peek");
    a.eq(acc[150 * 701 + 700], String.fromCharCode(0x41 + (150 % 26)), "rope middle");
    const digits = makeRope(Array.from({ length: 40 }, () => "0123456789".repeat(60)));
    const dw = [];
    for (let i = 0; i < digits.length; i++) dw.push(String.fromCharCode(0x30 + (i % 10)));
    a.deep(idxLoop(digits, digits.length), [undefined, undefined, ...dw, undefined, undefined], "rope read past the flatten threshold (len / 256 reads), every unit distinct from its neighbour");
});
t.test("out-of-range and non-int keys still reach the prototype", (a) => {
    String.prototype[20] = "P";
    String.prototype[1202] = "P";
    String.prototype["1.5"] = "F";
    try {
        const out = [];
        for (let k = 0; k < 3; k++) out.push(narrow[20], narrow[1], narrow["1"], narrow[1.5], narrow[-0], narrow[1.0]);
        a.deep(out.slice(0, 6), ["P", narrow.charAt(1), narrow.charAt(1), "F", narrow.charAt(0), narrow.charAt(1)]);
        a.eq(ropeOf([0x61, 0x62])[1202], "P", "rope out of range");
    } finally {
        delete String.prototype[20];
        delete String.prototype[1202];
        delete String.prototype["1.5"];
    }
    a.eq(narrow[20], undefined, "after delete");
});
t.test("one-unit string equality", (a) => {
    let n = 0;
    for (let i = 0; i < flat.length; i++) {
        if (flat[i] === '"') n += 1;
        if (flat[i] === "é") n += 10;
        if (flat[i] === "α") n += 100;
        if (flat[i] == "'") n += 1000;
        if (flat[i] !== flat.charAt(i)) n += 100000;
    }
    a.eq(n, 1111);
    const wideA = "Āa".slice(1), narrowA = "a";
    let eq = 0;
    for (let k = 0; k < 4; k++) eq += (wideA === narrowA) + (wideA == narrowA) + ("ab".slice(0, 1) === "a") + ("ab".slice(1) === "a");
    a.eq(eq, 12, "a wide-stored 'a' equals a narrow 'a'");
    a.ok("aa" !== "a" && "" !== "a" && "\ud800" !== "\udc00", "length and unit mismatches");
});
t.test("charCodeAt inline matches the builtin", (a) => {
    a.deep(ccaLoop(flat, units.length), wantCode(units), "flat");
    a.deep(ccaLoop(narrow, narrowUnits.length), wantCode(narrowUnits), "narrow");
    const r = ropeOf(units);
    a.deep(ccaLoop(r, r.length), wantCode(ropeUnits(units)), "rope (unflattened, then flattened)");
    a.deep(ccaLoop(r, r.length), wantCode(ropeUnits(units)), "rope again");
    const odd = [];
    for (let k = 0; k < 2; k++) odd.push(narrow.charCodeAt(), narrow.charCodeAt(1.7), narrow.charCodeAt("2"), narrow.charCodeAt(-0), narrow.charCodeAt(2 ** 31), new String(narrow).charCodeAt(1));
    a.deep(odd.slice(0, 6), [narrowUnits[0], narrowUnits[1], narrowUnits[2], narrowUnits[0], NaN, narrowUnits[1]]);
});
t.test("an overridden or getter charCodeAt is called", (a) => {
    const orig = String.prototype.charCodeAt;
    const seen = [];
    try {
        String.prototype.charCodeAt = function (i) { seen.push(i); return -1; };
        const got = [];
        for (let i = 0; i < 3; i++) got.push(narrow.charCodeAt(i));
        a.deep(got, [-1, -1, -1], "override result");
        a.deep(seen, [0, 1, 2], "override saw the calls");
        let gets = 0;
        Object.defineProperty(String.prototype, "charCodeAt", { configurable: true, get() { gets++; return orig; } });
        const g2 = [];
        for (let i = 0; i < 3; i++) g2.push(narrow.charCodeAt(i));
        a.deep(g2, narrowUnits.slice(0, 3), "getter returning the builtin");
        a.eq(gets, 3, "getter ran per call");
    } finally {
        Object.defineProperty(String.prototype, "charCodeAt", { value: orig, configurable: true, writable: true, enumerable: false });
    }
    const own = Object.assign(new String("xyz"), { charCodeAt: () => 7 });
    const g3 = [];
    for (let i = 0; i < 2; i++) g3.push(own.charCodeAt(i));
    a.deep(g3, [7, 7], "own method on a String object");
    a.eq(narrow.charCodeAt(0), narrowUnits[0], "restored");
});
await t.run();
