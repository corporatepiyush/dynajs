// flags: --std
// covers: dyna:cli.prompt
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_cli_prompt_term");
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

__kitS.section("hello world", () => {
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
});

__kitS.section("an unknown prompt key names the key:", () => {
    let msg = "";
    try { prompt("x", { defualt: "y" }); } catch (e) { msg = e.message; }
    assert(msg.indexOf("defualt") >= 0, "an unknown prompt key names the key: " + msg);
    assert(msg.indexOf("default") >= 0, "and the valid set: " + msg);
});

__kitS.section("an answer past 1 MiB without a newline throws RangeError", () => {
    let cls = "";
    withIO("a".repeat((1 << 20) + 2), () => {
        try { prompt("Q"); } catch (e) { cls = e.constructor.name; }
    });
    eq(cls, "RangeError", "an answer past 1 MiB without a newline throws RangeError");
});

__kitS.section("exactly 1 MiB + CRLF is accepted (the stripped CR does not count)", () => {
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
});
__kitS.end();

if (fails) {
    print("test_term: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_term failed");
}
print("test_term: " + n + " assertions, 0 failures");
