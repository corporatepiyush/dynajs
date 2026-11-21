// flags: --std
// covers: dyna:cli.ProgressBar
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_cli_progress_bar_term");
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

__kitS.section("update() returns this", () => {
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
});

__kitS.section("ProgressBar needs its opts object", () => {
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
});
__kitS.end();

if (fails) {
    print("test_term: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_term failed");
}
print("test_term: " + n + " assertions, 0 failures");
