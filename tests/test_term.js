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
import { StyleText, Styles, IsTTY, Columns, ColorDepth,
         prompt, confirm, select, keypress, ProgressBar, Spinner, Table } from "dyna:cli";
import * as std from "std";
import * as os from "os";

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

/* Emission rows in this file assert bytes literally, so FORCE_COLOR=1 pins
   emission for the whole run (restored at the end): the auto-dim DEFAULT --
   dimmed on a piped stdout or under NO_COLOR -- is asserted explicitly in its
   own section below, with the env swapped and restored around each row. */
const PIN_FC = std.getenv("FORCE_COLOR");
const PIN_NC = std.getenv("NO_COLOR");
std.setenv("FORCE_COLOR", "1");

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
    setenv("FORCE_COLOR", "1");   // back to the suite's emission pin
}

/* --------------------------------------------------------------- the module */

/* ------------------------------------------------- extended colors */

/* The dynamic color forms sit beside the 46 names: 256-color and truecolor
 * for foreground and background. The emitted SGR is the contract; a malformed
 * form and an out-of-range component are different refusals. */
eq(StyleText("256:200", "x"), E + "38;5;200mx" + E + "39m", "256-color foreground");
eq(StyleText("bg256:16", "x"), E + "48;5;16mx" + E + "49m", "256-color background");
eq(StyleText("256:0", "x"), E + "38;5;0mx" + E + "39m", "index 0 is in range");
eq(StyleText("256:255", "x"), E + "38;5;255mx" + E + "39m", "index 255 is in range");
eq(StyleText("#102030", "x"), E + "38;2;16;32;48mx" + E + "39m", "hex truecolor");
eq(StyleText("bg#102030", "x"), E + "48;2;16;32;48mx" + E + "49m", "hex truecolor background");
eq(StyleText("#ABCDEF", "x"), E + "38;2;171;205;239mx" + E + "39m", "hex digits are case-insensitive");
eq(StyleText("rgb:1,2,3", "x"), E + "38;2;1;2;3mx" + E + "39m", "rgb triple");
eq(StyleText("bgRgb:255,0,128", "x"), E + "48;2;255;0;128mx" + E + "49m", "bgRgb triple");
eq(StyleText(["red", "256:9"], "x"), E + "31m" + E + "38;5;9mx" + E + "39m" + E + "39m",
   "extended forms compose with named styles and close in reverse");
/* out-of-range numeric components are a RangeError... */
for (const s of ["256:256", "256:999", "bg256:300", "rgb:1,2,300", "rgb:256,0,0",
                 "bgRgb:0,0,999", "256:1234"]) {
    let e = null;
    try { StyleText(s, "x"); } catch (ex) { e = ex; }
    assert(e !== null && e.constructor.name === "RangeError",
           s + " is refused as a RangeError");
}
/* ...while malformed SHAPES read as unknown styles (TypeError). */
for (const s of ["256:", "256:-1", "256:1e2", "256: 1", "256:0x2", "bg256:",
                 "#12345", "#12345g", "#1234567", "rgb:1,2", "rgb:1,2,", "rgb:1,2,3,",
                 "rgb:1,2,3,4", "bgRgb:a,b,c", "bg#", "rgb:", "bg256:12a"]) {
    let e = null;
    try { StyleText(s, "x"); } catch (ex) { e = ex; }
    assert(e !== null && e.constructor.name === "TypeError",
           s + " is refused as a TypeError");
}
throws(() => StyleText("256:9\0evil", "x"),
       "an embedded NUL cannot truncate an extended form to a valid one");

/* --------------------------------------------------- auto-dim (gate) */

/* Emission is gated, validation is not: NO_COLOR or a non-TTY stdout drops
   the escapes and returns the text unchanged, while every refusal above
   still fires. FORCE_COLOR overrides both ways. */
{
    const hadFC = getenv_maybe("FORCE_COLOR"), hadNC = getenv_maybe("NO_COLOR");
    const { setenv, unsetenv } = std;
    unsetenv("FORCE_COLOR"); setenv("NO_COLOR", "1");
    eq(StyleText("red", "x"), "x", "NO_COLOR dims emission to plain text");
    eq(StyleText(["red", "bold"], "x"), "x", "a style array dims too");
    eq(StyleText("reset", "x"), "x", "and reset");
    eq(StyleText("256:200", "x"), "x", "and the extended colors");
    throws(() => StyleText("chartreuse", "x"), "dimming still validates style names");
    throws(() => StyleText("256:999", "x"), "and still refuses out-of-range indices");
    setenv("FORCE_COLOR", "1");
    assert(StyleText("red", "x").indexOf(E + "31m") === 0,
           "FORCE_COLOR re-enables emission over NO_COLOR");
    setenv("FORCE_COLOR", "0");
    eq(StyleText("red", "x"), "x", "FORCE_COLOR=0 refuses");
    setenv("FORCE_COLOR", "false");
    eq(StyleText("red", "x"), "x", "FORCE_COLOR=false refuses");
    unsetenv("FORCE_COLOR"); unsetenv("NO_COLOR");
    if (!IsTTY(1))
        eq(StyleText("red", "x"), "x", "a non-TTY stdout dims with a clean env");
    else
        assert(StyleText("red", "x").indexOf(E + "31m") === 0,
               "a TTY stdout emits with a clean env");
    setenv("NO_COLOR", "");
    if (!IsTTY(1))
        eq(StyleText("red", "x"), "x", "an EMPTY NO_COLOR is not the refusal (the pipe is)");
    restore_env("FORCE_COLOR", hadFC);
    restore_env("NO_COLOR", hadNC);
}

/* ------------------------------------------------------------ Table */

/* Column widths size to content, alignment pads per column, and the last
   column carries no trailing padding. */
eq(Table([["a", "b"], ["longer", 1]], { head: ["x", "y"], align: ["left", "right"] }),
   "x       y\n------  -\na       b\nlonger  1\n",
   "grid: head, dashed rule, aligned columns");
eq(Table([["a", 1], ["bbb", 22]], { align: ["right", "right"] }),
   "  a   1\nbbb  22\n", "right alignment pads on the left");
eq(Table([["a", "x"], ["ccc", "y"]], { align: ["center"] }),
   " a   x\nccc  y\n", "center alignment splits the pad (and unlisted columns are left)");
eq(Table([["n", 42.5]]), "n  42.5\n", "number cells render");
eq(Table([["éé", "x"], ["abc", "y"]]), "éé   x\nabc  y\n",
   "widths count code points, not bytes");
eq(Table([]), "", "no rows and no head is the empty string");
eq(Table([], { head: ["a", "b"] }), "a  b\n-  -\n", "a head without rows still renders");
/* tsv quotes nothing; cells keep whatever they carry */
eq(Table([["a", "b\tc"]], { format: "tsv" }), "a\tb\tc\n", "tsv joins raw cells with tabs");
eq(Table([["a"]], { head: ["h"], format: "tsv" }), "h\na\n", "tsv renders the head first");
/* csv quotes only what RFC 4180 requires */
eq(Table([["a,b", 'say "hi"'], ["nl\nx", 3]], { format: "csv" }),
   '"a,b","say ""hi"""\n' + '"nl\nx",3\n',
   "csv doubles embedded quotes and quotes separators/newlines");
eq(Table([["a"]], { head: ["h,1"], format: "csv" }), '"h,1"\na\n', "csv quotes a separator in the head");
/* refusals */
throws(() => Table("rows"), "rows must be an array");
throws(() => Table([[1, 2], [3]]), "a ragged row is refused");
throws(() => Table([[{}]]), "an object cell is refused");
throws(() => Table([["a"]], { align: ["justify"] }), "an unknown alignment is refused");
throws(() => Table([["a"]], { align: ["left", "right"] }), "an over-long align is refused");
throws(() => Table([["a"]], { align: "left" }), "align must be an array");
throws(() => Table([["a"]], { format: "xml" }), "an unknown format is refused");
throws(() => Table([["a"]], "bag"), "a non-object opts is refused");
throws(() => Table([["a"]], { head: [1] }), "a non-string head cell is refused");
throws(() => Table([["a"]], { head: "h" }), "head must be an array");
{
    let msg = "";
    try { Table([["a"]], { fmt: "csv" }); } catch (e) { msg = e.message; }
    assert(msg.indexOf("fmt") >= 0, "an unknown Table key names the key: " + msg);
    assert(msg.indexOf("head, align, format") >= 0, "and the valid set: " + msg);
}
{
    let msg = "";
    try { Table([[1, 2], [3, 4], ["a"]]); } catch (e) { msg = e.message; }
    assert(msg.indexOf("row 2") >= 0, "a ragged-row error names the row: " + msg);
}
{
    let msg = "";
    try { Table([["a", {}]]); } catch (e) { msg = e.message; }
    assert(msg.indexOf("(0, 1)") >= 0, "a bad-cell error names row and column: " + msg);
}

/* ------------------------------- input plumbing for the interactive rows */

/* The interactive APIs read fd 0 and write fd 1. The rows below swap both
   for temp files, so they are hermetic whether the harness pipes stdin or
   not -- and a swallowed FAIL message cannot hide inside a capture window. */
const TMPD = std.getenv("TMPDIR") || "/tmp";
const TMPN = "dj_e3_term_" + os.getpid();
/* `data` is either a JS string (written as UTF-8) or an array of raw byte
   values -- the decoder rows need bytes a JS string cannot express. */
function withIO(data, fn) {
    std.out.flush();
    const pin = TMPD + "/" + TMPN + ".in", pout = TMPD + "/" + TMPN + ".out";
    if (typeof data === "string") {
        const w = std.open(pin, "w"); w.puts(data); w.close();
    } else {
        const fd = os.open(pin, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644);
        os.write(fd, new Uint8Array(data).buffer);
        os.close(fd);
    }
    const fo = std.open(pout, "w");
    const fi = std.open(pin, "r");
    const sin = os.dup(0), sout = os.dup(1);
    os.dup2(fi.fileno(), 0);
    os.dup2(fo.fileno(), 1);
    let r, err;
    try { r = fn(); } catch (e) { err = e; }
    std.out.flush();
    os.dup2(sin, 0); os.dup2(sout, 1);
    os.close(sin); os.close(sout); fi.close(); fo.close();
    const rf = std.open(pout, "r"); const out = rf.readAsString(); rf.close();
    os.remove(pin); os.remove(pout);
    if (err) throw err;
    return { result: r, out: out };
}

/* ------------------------------------------------------- prompt/confirm */

eq(withIO("hello world\n", () => prompt("Q: ")).result, "hello world", "prompt reads a line");
eq(withIO("\n", () => prompt("Q", { default: "D" })).result, "D", "an empty answer takes the default");
eq(withIO("", () => prompt("Q", { default: "D" })).result, "D", "EOF takes the default");
eq(withIO("", () => prompt("Q")).result, "", "EOF without a default is the empty string");
eq(withIO("\n", () => prompt("Q")).result, "", "an empty answer without a default is too");
eq(withIO("x\r\n", () => prompt("Q")).result, "x", "CRLF loses the CR");
eq(withIO("no-newline", () => prompt("Q")).result, "no-newline", "a trailing partial line is an answer");
eq(withIO("a\nb\n", () => [prompt("1"), prompt("2")]).result.join(","), "a,b",
   "two lines make two answers");
eq(withIO("x\n", () => prompt("Name: ")).out, "Name: ", "the message is written verbatim");
throws(() => prompt(5), "a non-string message is refused");
throws(() => prompt("x", "bag"), "a non-object opts is refused");
throws(() => prompt("x", { default: 5 }), "a non-string default is refused");
{
    let msg = "";
    try { prompt("x", { defualt: "y" }); } catch (e) { msg = e.message; }
    assert(msg.indexOf("defualt") >= 0, "an unknown prompt key names the key: " + msg);
    assert(msg.indexOf("default") >= 0, "and the valid set: " + msg);
}
{
    /* the line cap: a stream with no newline must not grow without bound */
    let cls = "";
    withIO("a".repeat((1 << 20) + 2), () => {
        try { prompt("Q"); } catch (e) { cls = e.constructor.name; }
    });
    eq(cls, "RangeError", "an answer past 1 MiB without a newline throws RangeError");
}
{
    /* the cap counts the ANSWER, i.e. after a CRLF's CR is dropped: exactly
       1 MiB of content is accepted with EITHER line ending, and one content
       byte more is refused with either */
    let got = "?";
    withIO("a".repeat(1 << 20) + "\r\n", () => {
        try { got = String(prompt("Q").length); } catch (e) { got = "E:" + e.constructor.name; }
    });
    eq(got, String(1 << 20), "exactly 1 MiB + CRLF is accepted (the stripped CR does not count)");
    got = "?";
    withIO("a".repeat(1 << 20) + "\n", () => {
        try { got = String(prompt("Q").length); } catch (e) { got = "E:" + e.constructor.name; }
    });
    eq(got, String(1 << 20), "exactly 1 MiB + LF is accepted (same boundary)");
    let cls = "";
    withIO("a".repeat((1 << 20) + 1) + "\r\n", () => {
        try { prompt("Q"); } catch (e) { cls = e.constructor.name; }
    });
    eq(cls, "RangeError", "1 MiB + 1 content bytes + CRLF is refused (the cap counts the answer)");
}

for (const [inp, want] of [["y\n", true], ["Y\n", true], ["yes\n", true], ["YES\n", true],
                           ["Yes\n", true], ["yes\r\n", true], ["y", true],
                           ["n\n", false], ["\n", false], ["maybe\n", false], ["", false],
                           [" y\n", false], ["yes \n", false], ["yeah\n", false]])
    eq(withIO(inp, () => confirm("c?")).result, want,
       "confirm reads " + JSON.stringify(inp) + " as " + want);
eq(withIO("y\n", () => confirm("Sure? ")).out, "Sure? ", "confirm writes its message verbatim");
throws(() => confirm(5), "confirm refuses a non-string message");
{
    let cls = "";
    withIO("a".repeat((1 << 20) + 2), () => {
        try { confirm("Q"); } catch (e) { cls = e.constructor.name; }
    });
    eq(cls, "RangeError", "confirm shares the line cap");
}

/* ----------------------------------------------------------- keypress */

function keys(inp, n) {
    return withIO(inp, () => {
        const out = [];
        for (let i = 0; i < n; i++) {
            const k = keypress();
            if (k === null) { out.push("EOF"); break; }
            out.push(k);
        }
        return out;
    }).result;
}
{
    const ks = keys("a", 3);
    eq(ks.length, 2, "a plain key, then EOF");
    eq(ks[0].name, "a", "a plain character names itself");
    eq(ks[0].sequence, "a", "and carries its bytes");
    eq(JSON.stringify([ks[0].ctrl, ks[0].meta, ks[0].shift]), "[false,false,false]",
       "no modifiers on a plain key");
    eq(ks[1], "EOF", "keypress() is null at EOF");
}
{
    /* one pass over the decode table: control chords, CSI/ arrows and
       editing keys, CSI modifiers, the meta chord, enter/tab/backspace */
    const ks = keys("\x01\x1b[A\x1bOB\x1b[5~\x1b[3~\x1b[H\x1b[F\x1b[Z\x1b[1;5C\x1ba\rt\x7f", 20);
    const want = [
        ["a", "\u0001", true, false, false],
        ["up", "\u001b[A", false, false, false],
        ["down", "\u001bOB", false, false, false],
        ["pageup", "\u001b[5~", false, false, false],
        ["delete", "\u001b[3~", false, false, false],
        ["home", "\u001b[H", false, false, false],
        ["end", "\u001b[F", false, false, false],
        ["tab", "\u001b[Z", false, false, true],
        ["right", "\u001b[1;5C", true, false, false],
        ["a", "\u001ba", false, true, false],
        ["enter", "\r", false, false, false],
        ["t", "t", false, false, false],
        ["backspace", "\u007f", false, false, false],
        "EOF",
    ];
    eq(ks.length, want.length, "the decode table yields every event plus EOF");
    for (let i = 0; i < want.length; i++) {
        if (want[i] === "EOF") { eq(ks[i], "EOF", "input exhausted"); continue; }
        const [name, seq, ctrl, meta, shift] = want[i];
        eq(ks[i].name, name, "event " + i + " name");
        eq(ks[i].sequence, seq, "event " + i + " sequence is byte-exact");
        eq(JSON.stringify([ks[i].ctrl, ks[i].meta, ks[i].shift]),
           JSON.stringify([ctrl, meta, shift]), "event " + i + " modifiers");
    }
}
{
    const ks = keys([0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xFF], 5);
    eq(ks[0].name, "\u00e9", "a 2-byte UTF-8 character names itself whole");
    eq(ks[1].name, "\u20ac", "a 3-byte one too");
    eq(ks[2].name, null, "an invalid UTF-8 lead byte is an unknown key");
    eq(ks[2].sequence.length, 1, "and keeps its single byte as the sequence");
}
{
    /* a truncated UTF-8 lead must NOT swallow the byte after it: the
       non-continuation is its own key event, delivered next (a keystroke
       cannot vanish into a broken character) */
    const ks = keys([0xC3, 0x41, 0x42], 4);
    eq(ks[0].name, null, "the broken lead byte is an unknown key");
    eq(ks[0].sequence, "\uFFFD", "reported as U+FFFD");
    eq(ks[1].name, "A", "the byte after the broken lead is delivered, not swallowed");
    eq(ks[2].name, "B", "and the stream continues unharmed");
    eq(ks[3], "EOF", "with nothing lost and nothing extra");
}
{
    /* an ESC-run past the 15-byte sequence cap DELIVERS the tail as the next
       event(s): every byte read appears in some event's sequence */
    const ks = keys(new Array(20).fill(0x1B), 4);
    eq(ks[0].name, "escape", "a 20-byte escape run starts with an escape event");
    eq(ks[0].sequence.length, 15, "which reports exactly the 15 bytes it consumed");
    eq(ks[0].meta, true, "an ESC run is the meta chord");
    eq(ks[1].name, "escape", "the tail is delivered as its own event");
    eq(ks[1].sequence.length, 5, "5 more bytes: 20 consumed, 20 reported");
    eq(ks[2], "EOF", "and nothing after");
}
{
    const ks = keys("\x1b", 3);
    eq(ks[0].name, "escape", "a lone ESC decodes as Escape");
    eq(ks[0].meta, false, "without the meta chord");
    eq(ks[1], "EOF", "and consumes nothing after");
}
{
    const ks = keys("\x1b\x1b[Z\x1b\x1b", 4);
    eq(ks[0].name, "tab", "ESC ESC [ Z is shift+tab under meta");
    eq(JSON.stringify([ks[0].meta, ks[0].shift]), "[true,true]", "both modifiers");
    eq(ks[1].name, "escape", "ESC ESC alone is meta+escape");
    eq(ks[1].meta, true, "meta is set");
}
{
    const ks = keys("\x1b[9~\x1b[<1~", 4);
    eq(ks[0].name, null, "an unmapped CSI ~ form is an unknown key");
    eq(ks[0].sequence, "\u001b[9~", "with its bytes intact");
    eq(ks[1].name, null, "an unexpected parameter byte marks the sequence unknown");
}

/* ------------------------------------------------ select (uses ) */

{
    eq(withIO("\r", () => select("pick", ["one", "two"])).result, "one",
       "Enter takes the highlighted option");
    eq(withIO("\x1b[B\r", () => select("m", ["one", "two"])).result, "two",
       "Down moves and Enter takes");
    eq(withIO("\x1b[B\x1b[B\r", () => select("m", ["one", "two"])).result, "one",
       "Down wraps forward");
    eq(withIO("\x1b[A\r", () => select("m", ["one", "two"])).result, "two",
       "Up wraps backward");
    eq(withIO("\x1b", () => select("m", ["one", "two"])).result, null, "Escape cancels");
    eq(withIO("\x03", () => select("m", ["one", "two"])).result, null, "Ctrl-C cancels");
    eq(withIO("", () => select("m", ["one", "two"])).result, null, "EOF cancels");
    eq(withIO("z\x1b[Bq\r", () => select("m", ["one", "two"])).result, "two",
       "any other key is ignored");
    eq(withIO("\x1b[B\r", () => select("m", ["same", "same"])).result, "same",
       "duplicate options return the string");
    eq(withIO("\x1b[B\r", () => select("m", [""])).result, "",
       "an empty-string option is legal");
    /* the rendering: message, menu, marker, redraw over the rows */
    const out = withIO("\r", () => select("pick", ["one", "two"])).out;
    eq(out.split("\n")[0], "pick", "the message heads the menu");
    assert(out.indexOf("> one\x1b[K\n") > 0, "the marker sits on the highlighted row");
    assert(out.indexOf("  two\x1b[K") > 0, "the other rows are unmarked");
    assert(out.indexOf("\x1b[2A") > 0, "the redraw returns over the menu rows");
    throws(() => select("m", "x"), "options must be an array");
    throws(() => select("m", []), "and non-empty");
    throws(() => select("m", ["a", 5]), "of strings only");
    throws(() => select(5, ["a"]), "the message must be a string");
    throws(() => select("m"), "both arguments are required");
}

/* ---------------------------------------------- ProgressBar (output) */

{
    const r = withIO("", () => {
        const p = new ProgressBar({ total: 10 });
        const same = p.update(0) === p;
        p.update(5);
        p.update(10);
        let after = "";
        try { p.update(1); after = "NO-THROW"; } catch (e) { after = e.constructor.name; }
        return { same: same, after: after };
    });
    assert(r.result.same, "update() returns this");
    eq(r.result.after, "RangeError", "an update after completion is refused");
    assert(r.out.startsWith("\r["), "the bar redraws from column 0");
    assert(r.out.indexOf("[--------------------] 0% 0/10 ETA ?") >= 0, "the empty bar shape");
    assert(r.out.indexOf("[##########----------] 50% 5/10 ETA ") >= 0, "the half bar with an ETA");
    assert(r.out.indexOf("[####################] 100% 10/10 ETA 0s") >= 0, "the finished bar");
    eq(r.out.split("\n").length, 2, "exactly one line is terminated");
    assert(r.out.endsWith("\x1b[K\n"), "each redraw clears to the end of the line");
}
{   /* ctor and update refusals write nothing, so they run outside a capture */
    throws(() => new ProgressBar(), "ProgressBar needs its opts object");
    throws(() => new ProgressBar("x"), "a non-object opts is refused");
    throws(() => new ProgressBar({}), "a missing total is refused");
    throws(() => new ProgressBar({ total: "x" }), "a non-number total is refused");
    throws(() => new ProgressBar({ total: 0 }), "a zero total is refused");
    throws(() => new ProgressBar({ total: -1 }), "a negative total is refused");
    throws(() => new ProgressBar({ total: NaN }), "a NaN total is refused");
    throws(() => new ProgressBar({ total: Infinity }), "an infinite total is refused");
    {
        let msg = "";
        try { new ProgressBar({ totl: 1 }); } catch (e) { msg = e.message; }
        assert(msg.indexOf("totl") >= 0, "an unknown ProgressBar key names the key: " + msg);
        assert(msg.indexOf("total") >= 0, "and the valid set: " + msg);
    }
    const p = new ProgressBar({ total: 10 });
    throws(() => p.update(-1), "a negative n is refused");
    throws(() => p.update(11), "an n past total is refused");
    throws(() => p.update("5"), "a non-number n is refused");
    throws(() => p.update(), "an absent n is refused");
    eq(withIO("", () => p.update(10)).result, p, "the boundary n is accepted");
    throws(() => p.update(0), "and completes the bar for good");
}

/* -------------------------------------------------- Spinner (output) */

{
    const r = withIO("", () => {
        const s = new Spinner({ text: "work" });
        s.start().tick().tick().tick().tick();
        s.stop("done");
    });
    eq(r.out,
       "\r| work\x1b[K\r/ work\x1b[K\r- work\x1b[K\r\\ work\x1b[K\r| work\x1b[K\rdone\x1b[K\n",
       "the four frames cycle on one cleared line and stop leaves the final text");
}
{
    const r = withIO("", () => {
        const s = new Spinner();
        s.start();
        s.stop();
    });
    eq(r.out, "\r| \x1b[K\r\x1b[K", "an empty label renders, and a bare stop clears the line");
}
{   /* lifecycle refusals write nothing */
    const s = new Spinner();
    throws(() => s.tick(), "tick before start is refused");
    throws(() => s.stop(), "stop before start is refused");
    eq(withIO("", () => { s.start(); return s.tick(); }).result, s, "tick returns this");
    throws(() => s.start(), "a second start is refused");
    withIO("", () => s.stop());
    throws(() => s.tick(), "tick after stop is refused");
    throws(() => s.stop(), "stop after stop is refused");
    throws(() => s.start(), "start after stop is refused");
    throws(() => new Spinner({ txt: 1 }), "an unknown Spinner key is refused");
    throws(() => new Spinner({ text: 5 }), "a non-string label is refused");
    throws(() => new Spinner(5), "a non-object opts is refused");
    throws(() => withIO("", () => new Spinner().start().stop(5)), "stop(text) needs a string");
}

/* restore the env exactly as this file found it */
{
    const { setenv, unsetenv } = std;
    if (PIN_FC === undefined) unsetenv("FORCE_COLOR"); else setenv("FORCE_COLOR", PIN_FC);
    if (PIN_NC === undefined) unsetenv("NO_COLOR"); else setenv("NO_COLOR", PIN_NC);
}

if (fails) {
    print("test_term: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_term failed");
}
print("test_term: " + n + " assertions, 0 failures");
