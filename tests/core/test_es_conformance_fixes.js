// Spec behaviours fixed while taking test262 from 139 to 4 failures and enabling joint iteration, await-dictionary,
// legacy RegExp statics, immutable ArrayBuffers, string module export names and private-add-to-nonextensible.
// test262 is the bulk oracle; this file pins the cases it does not cover and the ones whose regression would be
// silent (a crash, a lost write, a value that looks plausible).
let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throwsT(f, ctor, m) {
    let e = null;
    try { f(); } catch (x) { e = x; }
    check(e instanceof ctor, m + " -- got " + (e ? e.constructor.name + ": " + e.message : "no exception"));
}

// EvalDeclarationInstantiation creates a var only when the binding does not exist: a second eval("var y") keeps y.
{
    function f() { eval("var y = 1"); eval("var y"); return y; }
    check(f() === 1, "a repeated eval var keeps the existing value");
    function g() { try { throw 8; } catch (x) { eval("var x = 42"); } return typeof x; }
    check(g() === "undefined", "eval var inside catch(x) creates the function-level binding (B.3.4)");
}

// The reference of an assignment target is resolved before the right-hand side runs.
{
    function t() { var x = 3; var inner = (function () { x *= (eval("var x = 2;"), 4); return x; })(); return [inner, x]; }
    const [inner, outer] = t();
    check(inner === 2 && outer === 12, "x *= (eval('var x'), 4) assigns the outer x");
    function u() { const c = 1; let r; c ||= (r = "rhs"); return r; }
    check(u() === undefined, "a short-circuiting ||= on a const neither throws nor runs the right-hand side");
    throwsT(() => { const c = 0; c ||= 1; }, TypeError, "a ||= that does assign a const throws TypeError");
}

// Annex B: a call expression as an assignment target is a runtime ReferenceError after the call, never a SyntaxError.
{
    let called = 0, rhs = 0;
    function f() { called++; return {}; }
    throwsT(() => { f() = (rhs++, 1); }, ReferenceError, "f() = 1 throws ReferenceError");
    check(called === 1 && rhs === 0, "the call runs, the right-hand side does not");
    throwsT(() => (0, eval)("'use strict'; f() = 1"), SyntaxError, "strict code keeps the early error");
    throwsT(() => (0, eval)("f`x` = 1"), SyntaxError, "a tagged template is not a call-expression target");
}

// Function(): parameters and body must each parse alone, so neither can close the other's syntax.
{
    throwsT(() => new Function("/*", "*/) {"), SyntaxError, "a comment cannot straddle parameters and body");
    throwsT(() => new Function("", "}); globalThis.__pwned = 1; (function(){"), SyntaxError, "the body cannot close the function");
    check(globalThis.__pwned === undefined, "the injected statement never ran");
    check(new Function("return typeof anonymous")() === "undefined", "the dynamic function has no name binding");
    check(String(new Function("a", "return a")).startsWith("function anonymous(a"), "toString still names it anonymous");
}

// Bound functions take the target's prototype, and their toString is valid NativeFunction syntax.
{
    function T() {}
    Object.setPrototypeOf(T, null);
    check(Object.getPrototypeOf(Function.prototype.bind.call(T, null)) === null, "a bound function inherits the target's [[Prototype]]");
    check(String(function named() {}.bind()) === "function () {\n    [native code]\n}", "bound toString omits the 'bound named' name");
    check(String(Math.max).startsWith("function max("), "plain natives keep their name");
}

// Object.defineProperties converts every descriptor before defining any.
{
    const o = {};
    throwsT(() => Object.defineProperties(o, { a: { value: 1 }, b: { get: 1 } }), TypeError, "a bad descriptor throws");
    check(!("a" in o), "nothing is defined when a later descriptor is invalid");
}

// Iterator.zip reads exactly iterCount padding values and closes the padding iterator (an infinite one used to OOM).
{
    let pulled = 0, closed = 0;
    const pad = { [Symbol.iterator]() { return { next() { pulled++; return { value: pulled, done: false }; }, return() { closed++; return {}; } }; } };
    const rows = [...Iterator.zip([[1], [2, 3]], { mode: "longest", padding: pad })];
    check(pulled === 2 && closed === 1, "padding read twice and closed once (" + pulled + "/" + closed + ")");
    check(JSON.stringify(rows) === "[[1,2],[1,3]]", "the first input is padded with padding[0]");
    const order = [];
    const mk = (tag) => ({ next() { return { value: 0, done: false }; }, return() { order.push(tag); return {}; } });
    const z = Iterator.zip([mk("a"), mk("b"), mk("c")]);
    z.next();
    z.return();
    check(order.join("") === "cba", "return() closes the inputs last to first");
    check(Object.getPrototypeOf(z) === Object.getPrototypeOf([].values().map((x) => x)), "zip results are Iterator Helpers");
    const k = Iterator.zipKeyed({ a: [1], b: [2] }).next().value;
    check(Object.getPrototypeOf(k) === null && k.a === 1 && k.b === 2, "zipKeyed yields null-prototype objects");
    throwsT(() => Iterator.zip([[1]], { mode: 1 }), TypeError, "a non-string mode is refused");
}

// Legacy RegExp statics follow every built-in match path and stop working after a subclass match.
{
    /(a)(b)?(c)/.exec("xxacyy");
    check(RegExp.$1 === "a" && RegExp.$2 === "" && RegExp.$3 === "c" && RegExp.lastParen === "c", "groups and lastParen");
    check(RegExp.leftContext === "xx" && RegExp.rightContext === "yy" && RegExp.input === "xxacyy", "contexts and input");
    "2026-10-26".replace(/(\d+)-(\d+)/, "_");
    check(RegExp.$1 === "2026" && RegExp.$2 === "10", "replace updates the statics");
    "a1b2".replace(/\d/g, "#");
    check(RegExp.lastMatch === "2", "a global replace leaves the last match");
    "q,w".split(/,/);
    check(RegExp.lastMatch === ",", "split updates the statics");
    class R extends RegExp {}
    new R("q").exec("q");
    throwsT(() => RegExp.$1, TypeError, "statics are unavailable after a subclass match");
    /z/.test("z");
    check(RegExp.lastMatch === "z", "the next legacy match restores them");
    throwsT(() => R.$1, TypeError, "a subclass receiver is refused");
    throwsT(() => new R("x").compile(), TypeError, "compile() is refused on subclass instances");
}

// Immutable ArrayBuffers.
{
    const src = new Uint8Array([1, 2, 3, 4]).buffer;
    const im = src.transferToImmutable();
    check(src.detached && im.immutable && im.byteLength === 4, "transferToImmutable detaches the source");
    const u = new Uint8Array(im);
    throwsT(() => { "use strict"; u[0] = 9; }, TypeError, "a strict element write throws");
    check(Reflect.set(u, 0, 9) === false && u[0] === 1, "Reflect.set reports false and the byte is unchanged");
    check(Reflect.defineProperty(u, 0, { value: 1 }) === true && Reflect.defineProperty(u, 0, { value: 2 }) === false, "defineProperty only accepts the same value");
    const d = Object.getOwnPropertyDescriptor(u, 0);
    check(d.writable === false && d.configurable === false && d.enumerable === true, "elements describe themselves as frozen");
    throwsT(() => u.fill(0), TypeError, "fill is refused");
    throwsT(() => u.set([7]), TypeError, "set is refused");
    throwsT(() => new DataView(im).setUint8(0, 1), TypeError, "DataView setters are refused");
    check(new DataView(im).getUint8(3) === 4, "reads still work");
    throwsT(() => Atomics.store(new Int32Array(new ArrayBuffer(4).transferToImmutable()), 0, 1), TypeError, "Atomics.store is refused");
    throwsT(() => im.transfer(), TypeError, "an immutable buffer cannot be transferred");
    throwsT(() => im.resize(0), TypeError, "or resized");
    const s = im.sliceToImmutable(1, 3);
    check(s.immutable && String([...new Uint8Array(s)]) === "2,3" && !im.detached, "sliceToImmutable copies without detaching");
    check(Reflect.set(u.subarray(1), 0, 9) === false, "views made from an immutable view stay immutable");
    const plain = new Uint8Array(4);
    plain[0] = 5;
    check(plain[0] === 5 && new ArrayBuffer(1).immutable === false, "ordinary buffers are unaffected");
}

// Private names cannot be added to non-extensible objects.
{
    class Base { constructor(o) { return o; } }
    class Stamp extends Base { #tag = 1; static has(o) { return #tag in o; } }
    throwsT(() => new Stamp(Object.preventExtensions({})), TypeError, "a private field on a non-extensible object throws");
    check(Stamp.has(new Stamp({})), "an extensible object still gets the field");
}

// Class field named get/set followed by a generator method on the next line is a field plus ASI.
{
    const C = eval("(class { set\n *a() {} })");
    check(new C().hasOwnProperty("set") && typeof C.prototype.a === "function", "set<LF>*a(){} is a field and a generator");
}

// Promise.allKeyed / allSettledKeyed.
{
    const s = Symbol("s");
    let got = null, settled = null;
    Promise.allKeyed({ b: Promise.resolve(2), a: 1, [s]: Promise.resolve("sym") }).then((o) => (got = o));
    Promise.allSettledKeyed({ ok: 1, bad: Promise.reject("why") }).then((o) => (settled = o));
    queueMicrotask(() => queueMicrotask(() => queueMicrotask(() => queueMicrotask(() => {
        check(got && Object.getPrototypeOf(got) === null && got.b === 2 && got.a === 1 && got[s] === "sym", "allKeyed resolves a keyed null-prototype object");
        check(got && Reflect.ownKeys(got).map(String).join(",") === "b,a,Symbol(s)", "key order follows the input");
        check(settled && settled.bad.status === "rejected" && settled.ok.value === 1, "allSettledKeyed records each outcome");
        print("test_es_conformance_fixes: " + n + " checks, " + fails + " failed");
        if (fails) throw new Error(fails + " failures");
    }))));
}
