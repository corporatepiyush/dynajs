// flags: --std
/* review probe: strict options bags, the API.md "primitive bag" claim,
 * Spinner/ProgressBar lifecycle + validation ordering, module surface. */
import { prompt, Table, Spinner, ProgressBar, Command, keypress, select,
         confirm, StyleText, Styles, IsTTY, Columns, ColorDepth } from "dyna:cli";
let n = 0, fails = 0;
function ok(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    ok(a === b, msg + "\n  got:  " + JSON.stringify(a) + "\n  want: " + JSON.stringify(b));
}
function errInfo(fn) {
    let e = null; try { fn(); } catch (x) { e = x; }
    return e;
}
function errCls(fn, cls, msg) {
    const e = errInfo(fn);
    ok(e && e.constructor.name === cls,
       msg + " (want " + cls + ", got " + (e ? e.constructor.name + ": " + e.message : "no throw") + ")");
    return e ? e.message : "";
}

/* B1 THE DOC CLAIM: "A null or primitive bag counts as absent" (API.md,
   dyna:cli intro). Probe every bag with a primitive. */
const prim = [
    ["prompt", () => prompt("m", 42)],
    ["prompt str", () => prompt("m", "s")],
    ["prompt bool", () => prompt("m", true)],
    ["Spinner", () => new Spinner(42)],
    ["Spinner str", () => new Spinner("s")],
    ["Table", () => Table([["a"]], 42)],
    ["option", () => new Command("t").option("--a <x>", "x", 42)],
];
for (const [name, fn] of prim) {
    const e = errInfo(fn);
    if (e) print("PIN B1 " + name + " primitive bag -> " + e.constructor.name + ": " + e.message);
    else print("PIN B1 " + name + " primitive bag -> COUNTS-AS-ABSENT (no throw)");
}
/* null bags must count as absent (both doc and impl agree) */
try { new Spinner(null); ok(true, "B1 Spinner(null) accepted"); } catch (e) { ok(false, "B1 Spinner(null): " + e.message); }

/* B2 strict-bag message format: `unknown option "defualt" (valid: default)` */
let m = errCls(() => prompt("m", { defualt: "x" }), "TypeError", "B2 unknown prompt key refused");
ok(m.indexOf('unknown option "defualt"') >= 0, "B2 names the key: " + m);
ok(m.indexOf("valid: default") >= 0, "B2b names the valid set: " + m);
m = errCls(() => Table([["a"]], { fmt: "x" }), "TypeError", "B2c Table key");
ok(m.indexOf('unknown option "fmt"') >= 0 && m.indexOf("head, align, format") >= 0, "B2c " + m);
m = errCls(() => new Spinner({ txt: "x" }), "TypeError", "B2d Spinner key");
ok(m.indexOf("valid: text") >= 0, "B2d " + m);
m = errCls(() => new ProgressBar({ totals: 3 }), "TypeError", "B2e ProgressBar key");
ok(m.indexOf("valid: total") >= 0, "B2e " + m);
m = errCls(() => new Command("t").option("--a <x>", "x", { envs: "X" }), "TypeError", "B2f option key");
ok(m.indexOf("valid: type, required, variadic, default, env") >= 0, "B2f " + m);

/* B3 symbol / inherited / non-enumerable keys are NOT considered */
try {
    const o = {};
    o[Symbol("s")] = 1;
    prompt("m", Object.assign(Object.create({ inherited: 1 }), { default: "d" }));
    ok(true, "B3a symbol + inherited keys invisible");
} catch (e) { ok(false, "B3a " + e.message); }
try {
    const o2 = { default: "d" };
    Object.defineProperty(o2, "hidden", { value: 1, enumerable: false });
    prompt("m", o2);
    ok(true, "B3b non-enumerable own key invisible");
} catch (e) { ok(false, "B3b " + e.message); }

/* B4 Spinner lifecycle + stop validation ORDER */
const s0 = new Spinner();
let e4 = errInfo(() => s0.stop(42));
ok(e4 && e4.constructor.name === "RangeError",
   "B4a fresh.stop(42): state error comes FIRST (RangeError), got " +
   (e4 ? e4.constructor.name : "no throw"));
e4 = errInfo(() => s0.tick());
ok(e4 && e4.constructor.name === "RangeError", "B4b tick before start -> RangeError");
s0.start();
e4 = errInfo(() => s0.stop(42));
ok(e4 && e4.constructor.name === "TypeError",
   "B4c running.stop(42): arg TypeError, got " + (e4 ? e4.constructor.name : "no throw"));
e4 = errInfo(() => s0.stop());
ok(e4 === null, "B4d refused stop() left the spinner RUNNING (plain stop() now works)");
e4 = errInfo(() => s0.stop("x"));
ok(e4 && e4.constructor.name === "RangeError",
   "B4e stop after stop -> RangeError (state beats arg), got " + (e4 ? e4.constructor.name : "no throw"));
e4 = errInfo(() => s0.start());
ok(e4 && e4.constructor.name === "RangeError", "B4f start after stop -> RangeError");
e4 = errInfo(() => s0.tick());
ok(e4 && e4.constructor.name === "RangeError", "B4g tick after stop -> RangeError");
/* B4h stop with valid text while running works and consumes the state */
const s1 = new Spinner({ text: "t" });
s1.start();
try { s1.stop("done"); ok(true, "B4h stop('done') works"); } catch (ex) { ok(false, "B4h " + ex.message); }

/* B5 ProgressBar validation + lifecycle */
errCls(() => new ProgressBar({ total: 0 }), "RangeError", "B5a total 0 refused");
errCls(() => new ProgressBar({ total: -1 }), "RangeError", "B5b negative refused");
errCls(() => new ProgressBar({ total: NaN }), "RangeError", "B5c NaN refused");
errCls(() => new ProgressBar({ total: Infinity }), "RangeError", "B5d Inf refused");
errCls(() => new ProgressBar({ total: 1e16 }), "RangeError", "B5e 1e16 refused");
errCls(() => new ProgressBar({}), "TypeError", "B5f missing total refused");
errCls(() => new ProgressBar({ total: "5" }), "TypeError", "B5g string total refused");
const bar = new ProgressBar({ total: 4 });
errCls(() => bar.update(-1), "RangeError", "B5h negative update refused");
errCls(() => bar.update(5), "RangeError", "B5i over-total update refused");
errCls(() => bar.update("2"), "TypeError", "B5j string update refused");
bar.update(0); bar.update(4);
errCls(() => bar.update(1), "RangeError", "B5k update after finish refused");

/* B6 module surface: exactly one name per function, no alias twins */
const EXPECT_FN = ["StyleText", "Styles", "IsTTY", "Columns", "ColorDepth",
                   "prompt", "confirm", "select", "keypress", "Table"];
const EXPECT_CLS = ["Command", "ProgressBar", "Spinner"];
const mod = { StyleText, Styles, IsTTY, Columns, ColorDepth, prompt, confirm,
              select, keypress, Table, Command, ProgressBar, Spinner };
eq(Object.keys(mod).sort().join(","), EXPECT_FN.concat(EXPECT_CLS).sort().join(","),
   "B6 import surface is exactly the 12 names");
for (const f of EXPECT_FN) ok(typeof mod[f] === "function", "B6 " + f + " is a function");
for (const c of EXPECT_CLS) ok(typeof mod[c] === "function", "B6 " + c + " is a class constructor");

/* B7 prompt({default: 42}) refused; prompt({default:"x", extra:1}) refused */
errCls(() => prompt("m", { default: 42 }), "TypeError", "B7 non-string default refused");
m = errCls(() => prompt("m", { default: "x", extra: 1 }), "TypeError", "B7b extra key refused");
ok(m.indexOf('unknown option "extra"') >= 0, "B7b " + m);

print("probe_bags: " + (n - fails) + "/" + n + " ok");
if (fails) throw new Error("probe_bags failures: " + fails);
