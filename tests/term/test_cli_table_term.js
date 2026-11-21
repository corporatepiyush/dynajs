// flags: --std
// covers: dyna:cli.Table
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_cli_table_term");
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

__kitS.section("x y - a b longer 1", () => {
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
eq(Table([["a", "b\tc"]], { format: "tsv" }), "a\tb\tc\n", "tsv joins raw cells with tabs");
eq(Table([["a"]], { head: ["h"], format: "tsv" }), "h\na\n", "tsv renders the head first");
eq(Table([["a,b", 'say "hi"'], ["nl\nx", 3]], { format: "csv" }),
   '"a,b","say ""hi"""\n' + '"nl\nx",3\n',
   "csv doubles embedded quotes and quotes separators/newlines");
eq(Table([["a"]], { head: ["h,1"], format: "csv" }), '"h,1"\na\n', "csv quotes a separator in the head");
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
});

__kitS.section("an unknown Table key names the key", () => {
    let msg = "";
    try { Table([["a"]], { fmt: "csv" }); } catch (e) { msg = e.message; }
    assert(msg.indexOf("fmt") >= 0, "an unknown Table key names the key: " + msg);
    assert(msg.indexOf("head, align, format") >= 0, "and the valid set: " + msg);
});

__kitS.section("row 2", () => {
    let msg = "";
    try { Table([[1, 2], [3, 4], ["a"]]); } catch (e) { msg = e.message; }
    assert(msg.indexOf("row 2") >= 0, "a ragged-row error names the row: " + msg);
});

__kitS.section("a bad-cell error names row and column", () => {
    let msg = "";
    try { Table([["a", {}]]); } catch (e) { msg = e.message; }
    assert(msg.indexOf("(0, 1)") >= 0, "a bad-cell error names row and column: " + msg);
});
__kitS.end();

if (fails) {
    print("test_term: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_term failed");
}
print("test_term: " + n + " assertions, 0 failures");
