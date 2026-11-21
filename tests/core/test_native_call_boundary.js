// covers: c:src/vm/interpreter.inc.c
// The JS<->C call boundary (js_call_dispatch, js_call_cfunc_generic in src/vm/interpreter.inc.c): calls from the tail-call handlers and from
// JS_Call/JS_CallFree go straight to the native function or the tail-call
// entry instead of through JS_CallInternal's interpreter prologue, and a plain
// or magic native called with at least its declared arg count skips the
// padding frame. Everything observable must stay as it was: the native frame
// in a backtrace, undefined padding when fewer
// args than declared are passed, magic dispatch, getters/setters and
// constructors still on the full path, a throw from a callback unwinding
// through the native that called it, stack overflow through native recursion
// becoming a RangeError, and a pending microtask still running only after
// a loop of native calls ends.
// Proven by planting faults in the fast paths: not linking the native frame
// (the "at get (native)" and "at map (native)" lines vanish), dropping the
// magic argument, and taking the fast path for a call with fewer args than
// declared; each turns this file red. Running the native in the caller's realm
// instead of the function's is not observable from one script (one realm).
const out = [];
function where(e) {
    return e.stack.split("\n").map((l) => l.trim()).filter((l) => l.startsWith("at ")).slice(0, 4)
        .map((l) => l.replace(/\(.*test_native_call_boundary\.js:(\d+):(\d+)\)/, "($1:$2)")).join(" | ");
}
function nativeThrowInLoop(n) {
    const m = new Map();
    for (let i = 0; i < n; i++) {
        if (i === n - 1) return m.get.call(null, 1);
        m.get(i);
    }
}
function callbackThrow(n) {
    const a = [1, 2, 3];
    let k = 0;
    for (let i = 0; i < n; i++) a.map((v) => { if (++k === n) throw new RangeError("cb"); return v; });
}
function deepThroughNative(d) { return [d].map((x) => deepThroughNative(x + 1))[0]; }
for (const [name, f] of [["nativeThrowInLoop", () => nativeThrowInLoop(500)], ["callbackThrow", () => callbackThrow(300)]]) {
    try { f(); out.push(name + " no-throw"); } catch (e) { out.push(name + " " + e.constructor.name + " " + e.message + " " + where(e)); }
}
try { deepThroughNative(0); out.push("deep no-throw"); } catch (e) { out.push("deep " + e.constructor.name); }

let s = 0;
for (let i = 0; i < 2000; i++) s += "abc".padStart(i & 7).length + "x".repeat(i & 3).length;
out.push("padding " + s);
let pads = "";
for (let i = 0; i < 300; i++) pads = "ab".padEnd(4) + "|" + "ab".padStart(5, "-");
out.push("padEnd " + JSON.stringify(pads));

const magics = [];
for (let i = 0; i < 300; i++) {
    const d = new Date(Date.UTC(2020, 1, 3, 4, 5, 6));
    magics[0] = d.getUTCFullYear(); magics[1] = d.getUTCMonth(); magics[2] = d.getUTCDate();
    magics[3] = d.getUTCHours(); magics[4] = d.getUTCMinutes(); magics[5] = d.getUTCSeconds();
    magics[6] = Math.max(i, 3, 2); magics[7] = Math.min(i, 3, 2);
    magics[8] = [1, 2, 3].includes(2); magics[9] = "abc".at(-1); magics[10] = String.fromCharCode(65, 66);
}
out.push("magic " + JSON.stringify(magics));

const got = [];
const o = { get g() { return 7; }, set s(v) { got.push(v); } };
for (let i = 0; i < 200; i++) { got[0] = Object.getOwnPropertyDescriptor(o, "g").get.call(o); Object.getOwnPropertyDescriptor(o, "s").set.call(o, i); }
out.push("accessors " + got[0] + " " + got.length + " " + got[got.length - 1]);
let ctorErr = "";
try { Map(); } catch (e) { ctorErr = e.constructor.name + " " + e.message; }
out.push("ctor-as-func " + ctorErr + " " + typeof new Map());

let order = "";
Promise.resolve().then(() => { order += "micro;"; });
for (let i = 0; i < 1000; i++) Math.abs(-i) && [].push.call([], i);
order += "loop;";
Promise.resolve().then(() => {
out.push("order " + order);

const want = [
    "nativeThrowInLoop TypeError Map object expected at get (native) | at call (native) | at nativeThrowInLoop (26:26) | at <anonymous> (36:71)",
    "callbackThrow RangeError cb at <anonymous> (33:83) | at map (native) | at callbackThrow (33:38) | at <anonymous> (36:116)",
    "deep RangeError",
    "padding 11500",
    "padEnd \"ab  |---ab\"",
    "magic [2020,1,3,4,5,6,299,2,true,\"c\",\"AB\"]",
    "accessors 7 201 199",
    "ctor-as-func TypeError must be called with new object",
    "order loop;micro;",
];
const gotStr = out.join("\n"), wantStr = want.join("\n");
if (gotStr !== wantStr) {
    console.log("FAIL got:\n" + gotStr + "\nwant:\n" + wantStr);
    throw new Error("native call boundary");
}
console.log("ok test_native_call_boundary");
});
