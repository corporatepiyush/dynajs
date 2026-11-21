// flags: --std
// covers: dyna:cli.StyleText
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_cli_style_text_term");
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

const PIN_FC = std.getenv("FORCE_COLOR");
const PIN_NC = std.getenv("NO_COLOR");
std.setenv("FORCE_COLOR", "1");

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
function getenv_maybe(k) {
    const v = std.getenv(k);
    return v === undefined ? null : v;
}
function restore_env(k, v) {
    if (v === null) std.unsetenv(k); else std.setenv(k, v);
}
const TMPD = std.getenv("TMPDIR") || "/tmp";
const TMPN = "dj_e3_term_" + os.getpid();
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
eq(StyleText(["blue", "dim"], "det"), StyleText(["blue", "dim"], "det"),
   "the same call twice emits the same bytes");

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
throws(() => StyleText("red\x00evil", "x"), "a NUL does not let a name match its prefix");
throws(() => StyleText("\x00red", "x"), "a leading NUL is not the empty-style wildcard");

eq(StyleText("red", "\x1B[2J"), E + "31m" + E + "2J" + E + "39m",
   "control bytes pass through verbatim (injection is the caller's problem)");
eq(StyleText("red", "\x1B[31malready\x1B[0m"),
   E + "31m" + E + "31m" + "already" + E + "0m" + E + "39m",
   "embedded SGR is not escaped either");
eq(StyleText("red", "a\x00b"), E + "31m" + "a\x00b" + E + "39m",
   "text after an embedded NUL survives");
throws(() => StyleText("red", 42), "numbers are refused as text");
throws(() => StyleText("red", null), "null is refused as text");
throws(() => StyleText("red", undefined), "undefined is refused as text");
throws(() => StyleText("red", [1, 2]), "arrays are refused as text");
throws(() => StyleText("red", Symbol("s")), "a Symbol is refused as text");
}

__kitS.section("an unknown style is a TypeError like Node", () => {
    let e = null;
    try { StyleText("chartreuse", "x"); } catch (ex) { e = ex; }
    assert(e !== null && e.constructor.name === "TypeError",
           "an unknown style is a TypeError like Node");
});

__kitS.section("a 1MB text gains only the four codes", () => {
    const big = "y".repeat(1 << 20);
    const r = StyleText(["green", "bold"], big);
    eq(r.length, big.length + 5 + 4 + 5 + 5, "a 1MB text gains only the four codes");
    assert(r.slice(9, 9 + big.length) === big, "the 1MB payload is verbatim");
});

__kitS.section("a negative Proxy length degrades to plain text", () => {
    const p = new Proxy(["red"], { get(t, k, r) { return k === "length" ? -5 : Reflect.get(t, k, r); } });
    eq(StyleText(p, "x"), "x", "a negative Proxy length degrades to plain text");
});

__kitS.section("256-color foreground", () => {
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
for (const s of ["256:256", "256:999", "bg256:300", "rgb:1,2,300", "rgb:256,0,0",
                 "bgRgb:0,0,999", "256:1234"]) {
    let e = null;
    try { StyleText(s, "x"); } catch (ex) { e = ex; }
    assert(e !== null && e.constructor.name === "RangeError",
           s + " is refused as a RangeError");
}
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
});

__kitS.section("NO_COLOR dims emission to plain text", () => {
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
});
__kitS.end();

if (fails) {
    print("test_term: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_term failed");
}
print("test_term: " + n + " assertions, 0 failures");
