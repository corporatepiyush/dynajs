// flags: --std
/* test_term.js -- the dyna:cli terminal surface: StyleText, Styles, IsTTY,
 * Columns, ColorDepth (the Command parser has its own suite, test_cli.js).
 *
 * The emitted escape sequences ARE the spec, so every styling row asserts
 * bytes literally. The env rows use std.setenv and RESTORE it, so the suite
 * is hermetic whatever order the harness runs files in. PTY-only branches
 * (TERM/COLORTERM on a real TTY) are deliberately NOT asserted here: the
 * harness pipes stdout, and a row that only passes under a TTY is a flake
 * generator -- they are pinned in probes/tty_path_pty.js instead.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_term.js
 */
import { StyleText, Styles, IsTTY, Columns, ColorDepth } from "dyna:cli";
import * as std from "std";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const E = "\x1B[";

/* ------------------------------------------------------------ the byte table */

/* The on/off codes are the contract, verbatim from the table in dyna-term.c:
 * modifiers give back their own bit (bold->22), colors give back 39,
 * backgrounds 49, and "reset" opens 0m and CLOSES with a second 0m. The set
 * is Node v22's 46 names (bytes verified against node v22.23.1), so it is
 * larger than the old 24-style table and ordered differently: blink, hidden,
 * doubleunderline sit with the modifiers; gray moved after the plain colors;
 * framed/overlined and the bright families close the list. */
const TABLE = [
    ["reset", "0m", "0m"],
    ["bold", "1m", "22m"],
    ["dim", "2m", "22m"],
    ["italic", "3m", "23m"],
    ["underline", "4m", "24m"],
    ["blink", "5m", "25m"],
    ["inverse", "7m", "27m"],
    ["hidden", "8m", "28m"],
    ["strikethrough", "9m", "29m"],
    ["doubleunderline", "21m", "24m"],
    ["black", "30m", "39m"],
    ["red", "31m", "39m"],
    ["green", "32m", "39m"],
    ["yellow", "33m", "39m"],
    ["blue", "34m", "39m"],
    ["magenta", "35m", "39m"],
    ["cyan", "36m", "39m"],
    ["white", "37m", "39m"],
    ["bgBlack", "40m", "49m"],
    ["bgRed", "41m", "49m"],
    ["bgGreen", "42m", "49m"],
    ["bgYellow", "43m", "49m"],
    ["bgBlue", "44m", "49m"],
    ["bgMagenta", "45m", "49m"],
    ["bgCyan", "46m", "49m"],
    ["bgWhite", "47m", "49m"],
    ["framed", "51m", "54m"],
    ["overlined", "53m", "55m"],
    ["gray", "90m", "39m"],
    ["grey", "90m", "39m"],
    ["redBright", "91m", "39m"],
    ["greenBright", "92m", "39m"],
    ["yellowBright", "93m", "39m"],
    ["blueBright", "94m", "39m"],
    ["magentaBright", "95m", "39m"],
    ["cyanBright", "96m", "39m"],
    ["whiteBright", "97m", "39m"],
    ["bgGray", "100m", "49m"],
    ["bgGrey", "100m", "49m"],
    ["bgRedBright", "101m", "49m"],
    ["bgGreenBright", "102m", "49m"],
    ["bgYellowBright", "103m", "49m"],
    ["bgBlueBright", "104m", "49m"],
    ["bgMagentaBright", "105m", "49m"],
    ["bgCyanBright", "106m", "49m"],
    ["bgWhiteBright", "107m", "49m"],
];
{
    const list = Styles();
    assert(Array.isArray(list), "Styles() returns an array");
    eq(list.length, 46, "Styles() enumerates Node v22's 46 names");
    eq(list.length, TABLE.length, "Styles() enumerates the whole table");
    for (let i = 0; i < TABLE.length; i++) {
        const [name, on, off] = TABLE[i];
        eq(list[i], name, "Styles()[" + i + "] is " + name);
        eq(StyleText(name, "x"), E + on + "x" + (off ? E + off : ""),
           name + " emits its exact SGR pair");
    }
}

/* ------------------------------------------------------- composition shapes */

eq(StyleText(["red", "bold", "underline"], "x"),
   E + "31m" + E + "1m" + E + "4m" + "x" + E + "24m" + E + "22m" + E + "39m",
   "an array opens in order and closes in REVERSE");
eq(StyleText([], "x"), "x", "an empty style array is plain text");
eq(StyleText("red", ""), E + "31m" + E + "39m", "empty text still brackets");
eq(StyleText("red", StyleText("bold", "x")),
   E + "31m" + E + "1m" + "x" + E + "22m" + E + "39m",
   "chaining nests inner-on-outer");
eq(StyleText(["red", "red"], "x"), E + "31m" + E + "31m" + "x" + E + "39m" + E + "39m",
   "a repeated style doubles BOTH halves");
eq(StyleText("reset", "x"), E + "0m" + "x" + E + "0m",
   "reset closes with a second 0m (Node's shape)");
/* Determinism: no hidden state may make the same call disagree with itself. */
eq(StyleText(["blue", "dim"], "det"), StyleText(["blue", "dim"], "det"),
   "the same call twice emits the same bytes");

/* ---------------------------------------------------------------- refusals */

throws(() => StyleText("chartreuse", "x"), "an unknown style is REFUSED, not ignored");
throws(() => StyleText("Red", "x"), "style names are case-sensitive");
throws(() => StyleText(" red", "x"), "no trimming either");
throws(() => StyleText("", "x"), "the empty name is refused");
throws(() => StyleText("red"), "StyleText needs both arguments");
throws(() => StyleText(), "including with no arguments at all");
throws(() => StyleText(31, "x"), "a number is not a style (no implicit stringify)");
throws(() => StyleText({ length: 1, 0: "red" }, "x"),
       "an array-LIKE is refused: only real arrays compose");
throws(() => StyleText(["red", 42], "x"), "a non-string element is refused");
throws(() => StyleText([, "red"], "x"), "a hole is refused (as undefined)");
throws(() => StyleText(["red", null], "x"), "a null element is refused");
/* A NUL in the name must not truncate to a valid prefix (F2 regression). */
throws(() => StyleText("red\x00evil", "x"), "a NUL does not let a name match its prefix");
throws(() => StyleText("\x00red", "x"), "a leading NUL is not the empty-style wildcard");

/* -------------------------------------------------- text passthrough shapes */

/* Injection is pinned as a CONTRACT: the text reaches the terminal verbatim,
 * exactly like Node's util.styleText. Callers sanitize; the styler does not. */
eq(StyleText("red", "\x1B[2J"), E + "31m" + E + "2J" + E + "39m",
   "control bytes pass through verbatim (injection is the caller's problem)");
eq(StyleText("red", "\x1B[31malready\x1B[0m"),
   E + "31m" + E + "31m" + "already" + E + "0m" + E + "39m",
   "embedded SGR is not escaped either");
/* A JS string may hold NUL; styling must not silently drop the tail (F1). */
eq(StyleText("red", "a\x00b"), E + "31m" + "a\x00b" + E + "39m",
   "text after an embedded NUL survives");
/* Non-string text is REFUSED with a TypeError (Node's ERR_INVALID_ARG_TYPE
 * class): a styled "null" in a terminal is a bug nobody sees. The old
 * ToString coercion of 42/null/undefined/arrays is gone. */
throws(() => StyleText("red", 42), "numbers are refused as text");
throws(() => StyleText("red", null), "null is refused as text");
throws(() => StyleText("red", undefined), "undefined is refused as text");
throws(() => StyleText("red", [1, 2]), "arrays are refused as text");
throws(() => StyleText("red", Symbol("s")), "a Symbol is refused as text");
/* The refusal classes: an unknown style is a TypeError like Node, not a
 * RangeError (both suites pin this so neither module can regress it). */
{
    let e = null;
    try { StyleText("chartreuse", "x"); } catch (ex) { e = ex; }
    assert(e !== null && e.constructor.name === "TypeError",
           "an unknown style is a TypeError like Node");
}
/* A 1MB payload must round-trip byte-exactly (builder bounds, not just shapes). */
{
    const big = "y".repeat(1 << 20);
    const r = StyleText(["green", "bold"], big);
    eq(r.length, big.length + 5 + 4 + 5 + 5, "a 1MB text gains only the four codes");
    assert(r.slice(9, 9 + big.length) === big, "the 1MB payload is verbatim");
}
/* A Proxy reporting a negative length styles nothing rather than misbehaving. */
{
    const p = new Proxy(["red"], { get(t, k, r) { return k === "length" ? -5 : Reflect.get(t, k, r); } });
    eq(StyleText(p, "x"), "x", "a negative Proxy length degrades to plain text");
}

/* ------------------------------------------------------------------- IsTTY */

assert(typeof IsTTY() === "boolean", "IsTTY() defaults to stdout and returns a boolean");
eq(IsTTY(-1), false, "a negative fd is never a TTY (EBADF, not a crash)");
eq(IsTTY(1e9), false, "an absurd fd is false, not a crash");
throws(() => IsTTY(Symbol("s")), "a Symbol fd is refused");

/* ------------------------------------------------- Columns (COLUMNS env) */

/* The whole value must parse: "1e9" collapsing to 1 would silently wreck any
 * layout built on the width (F3 regression). Bounds are exclusive (0,100000). */
{
    const { setenv, unsetenv } = std;
    const had = getenv_maybe("COLUMNS");
    const cols = () => Columns();
    setenv("COLUMNS", "123");  eq(cols(), 123, "COLUMNS overrides the fallback");
    setenv("COLUMNS", "99999"); eq(cols(), 99999, "up to the exclusive bound");
    setenv("COLUMNS", "100000"); eq(cols(), 80, "the bound itself falls back");
    setenv("COLUMNS", "0");    eq(cols(), 80, "zero is not a width");
    setenv("COLUMNS", "-5");   eq(cols(), 80, "nor is a negative");
    setenv("COLUMNS", "1e9");  eq(cols(), 80, "a partial parse is refused (1e9 is not 1)");
    setenv("COLUMNS", "42x");  eq(cols(), 80, "trailing junk is refused");
    setenv("COLUMNS", "abc");  eq(cols(), 80, "letters are refused");
    setenv("COLUMNS", "");     eq(cols(), 80, "an empty value falls back");
    unsetenv("COLUMNS");       assert(Number.isInteger(cols()) && cols() > 0,
                                     "unset COLUMNS yields the positive fallback");
    restore_env("COLUMNS", had);
}
function getenv_maybe(k) {
    const v = std.getenv(k);
    return v === undefined ? null : v;
}
function restore_env(k, v) {
    if (v === null) std.unsetenv(k); else std.setenv(k, v);
}

/* --------------------------------------------------- ColorDepth (env matrix) */

{
    const { setenv, unsetenv } = std;
    const hadFC = getenv_maybe("FORCE_COLOR"), hadNC = getenv_maybe("NO_COLOR");
    const depth = () => ColorDepth(1);
    unsetenv("FORCE_COLOR"); unsetenv("NO_COLOR");
    const d = depth();
    assert([0, 4, 8, 24].indexOf(d) >= 0, "ColorDepth is 0, 4, 8 or 24 (got " + d + ")");
    if (!IsTTY(1)) eq(d, 0, "a piped stdout reports no colour");
    /* FORCE_COLOR overrides even the TTY probe; NO_COLOR never overrides it. */
    setenv("FORCE_COLOR", "0");     eq(depth(), 0, "FORCE_COLOR=0 refuses");
    setenv("FORCE_COLOR", "false"); eq(depth(), 0, "and so does false");
    setenv("FORCE_COLOR", "1");     eq(depth(), 4, "1 is 16-colour");
    setenv("FORCE_COLOR", "true");  eq(depth(), 4, "true too");
    setenv("FORCE_COLOR", "9");     eq(depth(), 4, "any other non-empty is level 1");
    setenv("FORCE_COLOR", "2");     eq(depth(), 8, "2 is 256-colour");
    setenv("FORCE_COLOR", "3");     eq(depth(), 24, "3 is truecolor");
    setenv("NO_COLOR", "1");        eq(depth(), 24, "FORCE_COLOR beats NO_COLOR");
    unsetenv("FORCE_COLOR");
    eq(depth(), 0, "NO_COLOR alone refuses on any stream");
    setenv("NO_COLOR", ""); unsetenv("NO_COLOR");
    eq(depth(), 0, "an EMPTY NO_COLOR is not a refusal (non-TTY is 0 anyway)");
    restore_env("FORCE_COLOR", hadFC);
    restore_env("NO_COLOR", hadNC);
}
/* FORCE_COLOR ignores the fd entirely -- forcing is the point -- while a clean
 * env reports 0 for any fd that cannot be a terminal. */
{
    const { setenv, unsetenv } = std;
    setenv("FORCE_COLOR", "2");
    eq(ColorDepth(-1), 8, "FORCE_COLOR applies whatever the fd is");
    unsetenv("FORCE_COLOR");
    eq(ColorDepth(-1), 0, "a nonexistent fd sees no colour on a clean env");
}

/* --------------------------------------------------------------- the module */

if (fails) {
    print("test_term: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_term failed");
}
print("test_term: " + n + " assertions, 0 failures");
