// flags: --std
/* review probe: StyleText extended colors, error CLASS contract, and the
 * NO_COLOR / FORCE_COLOR / non-tty emission matrix with env-invariant
 * validation. */
import { StyleText } from "dyna:cli";
import * as std from "std";
let n = 0, fails = 0;
function ok(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    ok(a === b, msg + "\n  got:  " + JSON.stringify(a) + "\n  want: " + JSON.stringify(b));
}
function errCls(fn, cls, msg) {
    let e = null; try { fn(); } catch (x) { e = x; }
    ok(e && e.constructor.name === cls,
       msg + " (want " + cls + ", got " + (e ? e.constructor.name + ": " + e.message : "no throw") + ")");
}

/* X1 the pinned refusal class: "256:1234" is a RANGE error, not TypeError */
errCls(() => StyleText("256:1234", "x"), "RangeError", "X1a 256:1234 -> RangeError (not TypeError)");
errCls(() => StyleText("256:300", "x"), "RangeError", "X1b 256:300 -> RangeError");
errCls(() => StyleText("256:256", "x"), "RangeError", "X1c 256:256 -> RangeError");
errCls(() => StyleText("bg256:999", "x"), "RangeError", "X1d bg256:999 -> RangeError");
errCls(() => StyleText("256:99999", "x"), "RangeError", "X1e 256:99999 (5 digits) -> RangeError");
/* malformed SHAPE is TypeError, distinct from out-of-range */
errCls(() => StyleText("256:-1", "x"), "TypeError", "X1f 256:-1 is a malformed shape -> TypeError");
errCls(() => StyleText("256:0x10", "x"), "TypeError", "X1g 256:0x10 malformed -> TypeError");
errCls(() => StyleText("256:", "x"), "TypeError", "X1h 256: empty body malformed -> TypeError");
errCls(() => StyleText("256:12 ", "x"), "TypeError", "X1i trailing space malformed -> TypeError");

/* X2 truecolor range vs shape */
errCls(() => StyleText("rgb:1,2,300", "x"), "RangeError", "X2a rgb component 300 -> RangeError");
errCls(() => StyleText("bgRgb:256,0,0", "x"), "RangeError", "X2b bgRgb component 256 -> RangeError");
errCls(() => StyleText("rgb:1,2", "x"), "TypeError", "X2c rgb two components -> TypeError");
errCls(() => StyleText("rgb:1,2,3,4", "x"), "TypeError", "X2d rgb four components -> TypeError");
errCls(() => StyleText("rgb:1,,3", "x"), "TypeError", "X2e rgb empty component -> TypeError");
errCls(() => StyleText("#12345", "x"), "TypeError", "X2f short hex -> TypeError");
errCls(() => StyleText("#12345g", "x"), "TypeError", "X2g bad hex digit -> TypeError");
errCls(() => StyleText("bg#12345", "x"), "TypeError", "X2h short bg hex -> TypeError");

/* X3 the emission matrix: same bytes, every environment. Validation must be
   IDENTICAL in every one of them (env-independence). */
const FC = std.getenv("FORCE_COLOR"), NC = std.getenv("NO_COLOR");
const E = "\x1b[";
function matrix(setup, name, emit) {
    std.unsetenv("FORCE_COLOR"); std.unsetenv("NO_COLOR");
    setup();
    eq(StyleText("red", "x"), emit ? E + "31mx" + E + "39m" : "x", "X3 " + name + " emission");
    eq(StyleText(["bold", "256:7"], "y"), emit ? E + "1m" + E + "38;5;7my" + E + "39m" + E + "22m" : "y",
       "X3 " + name + " extended emission");
    /* validation env-independence: a typo throws in EVERY environment */
    errCls(() => StyleText("nope", "x"), "TypeError", "X3 " + name + " unknown style still throws");
    errCls(() => StyleText("256:400", "x"), "RangeError", "X3 " + name + " out-of-range still RangeError");
}
matrix(() => {}, "clean env on piped stdout", false);                       // non-tty dims
matrix(() => std.setenv("NO_COLOR", "1"), "NO_COLOR=1", false);
matrix(() => std.setenv("NO_COLOR", "yes"), "NO_COLOR=yes", false);
matrix(() => std.setenv("NO_COLOR", ""), "NO_COLOR=empty (not a refusal)", false); // piped stdout still dims
matrix(() => std.setenv("FORCE_COLOR", "1"), "FORCE_COLOR=1", true);
matrix(() => std.setenv("FORCE_COLOR", "yes"), "FORCE_COLOR=yes", true);
matrix(() => std.setenv("FORCE_COLOR", "2"), "FORCE_COLOR=2 (non-empty, not 0/false)", true);
matrix(() => std.setenv("FORCE_COLOR", "0"), "FORCE_COLOR=0 refuses", false);
matrix(() => std.setenv("FORCE_COLOR", "false"), "FORCE_COLOR=false refuses", false);
matrix(() => std.setenv("FORCE_COLOR", "FALSE"), "FORCE_COLOR=FALSE is NOT 'false' -> emits", true);
matrix(() => { std.setenv("FORCE_COLOR", "1"); std.setenv("NO_COLOR", "1"); },
       "FORCE_COLOR beats NO_COLOR", true);
matrix(() => { std.setenv("FORCE_COLOR", "0"); std.setenv("NO_COLOR", ""); },
       "FORCE_COLOR=0 beats empty NO_COLOR", false);
/* restore */
std.unsetenv("FORCE_COLOR"); std.unsetenv("NO_COLOR");
if (FC !== undefined) std.setenv("FORCE_COLOR", FC);
if (NC !== undefined) std.setenv("NO_COLOR", NC);

/* X4 text bytes survive verbatim (escapes inside text are just text) */
std.setenv("FORCE_COLOR", "1");
eq(StyleText("red", "\x1b[2Ja\x00\u00e9\U0001f600"),
   E + "31m" + "\x1b[2Ja\x00\u00e9\U0001f600" + E + "39m",
   "X4 text with ESC/NUL/UTF-8/emoji is verbatim");

/* X5 open/close snapshot: a mutating getter must not desync the closes */
const arr = ["red"];
let reads = 0;
Object.defineProperty(arr, 0, { get() { reads++; return reads > 1 ? "bold" : "red"; },
                                enumerable: true, configurable: true });
eq(StyleText(arr, "z"), E + "31mz" + E + "39m",
   "X5 close pass uses the OPEN snapshot (getter re-read would close 22m)");
std.unsetenv("FORCE_COLOR");

/* X6 empty style array and empty text */
std.setenv("FORCE_COLOR", "1");
eq(StyleText([], "x"), "x", "X6a empty array is plain");
eq(StyleText("red", ""), E + "31m" + E + "39m", "X6b empty text still brackets");
std.unsetenv("FORCE_COLOR");

print("probe_style: " + (n - fails) + "/" + n + " ok");
if (fails) throw new Error("probe_style failures: " + fails);
