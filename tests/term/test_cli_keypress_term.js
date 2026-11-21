// flags: --std
// covers: dyna:cli.keypress
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_cli_keypress_term");
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

__kitS.section("a plain key, then EOF", () => {
    const ks = keys("a", 3);
    eq(ks.length, 2, "a plain key, then EOF");
    eq(ks[0].name, "a", "a plain character names itself");
    eq(ks[0].sequence, "a", "and carries its bytes");
    eq(JSON.stringify([ks[0].ctrl, ks[0].meta, ks[0].shift]), "[false,false,false]",
       "no modifiers on a plain key");
    eq(ks[1], "EOF", "keypress() is null at EOF");
});

__kitS.section("the decode table yields every event plus EOF", () => {
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
});

__kitS.section("a 2-byte UTF-8 character names itself whole", () => {
    const ks = keys([0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xFF], 5);
    eq(ks[0].name, "\u00e9", "a 2-byte UTF-8 character names itself whole");
    eq(ks[1].name, "\u20ac", "a 3-byte one too");
    eq(ks[2].name, null, "an invalid UTF-8 lead byte is an unknown key");
    eq(ks[2].sequence.length, 1, "and keeps its single byte as the sequence");
});

__kitS.section("the broken lead byte is an unknown key", () => {
    const ks = keys([0xC3, 0x41, 0x42], 4);
    eq(ks[0].name, null, "the broken lead byte is an unknown key");
    eq(ks[0].sequence, "\uFFFD", "reported as U+FFFD");
    eq(ks[1].name, "A", "the byte after the broken lead is delivered, not swallowed");
    eq(ks[2].name, "B", "and the stream continues unharmed");
    eq(ks[3], "EOF", "with nothing lost and nothing extra");
});

__kitS.section("a 20-byte escape run starts with an escape event", () => {
    const ks = keys(new Array(20).fill(0x1B), 4);
    eq(ks[0].name, "escape", "a 20-byte escape run starts with an escape event");
    eq(ks[0].sequence.length, 15, "which reports exactly the 15 bytes it consumed");
    eq(ks[0].meta, true, "an ESC run is the meta chord");
    eq(ks[1].name, "escape", "the tail is delivered as its own event");
    eq(ks[1].sequence.length, 5, "5 more bytes: 20 consumed, 20 reported");
    eq(ks[2], "EOF", "and nothing after");
});

__kitS.section("a lone ESC decodes as Escape", () => {
    const ks = keys("\x1b", 3);
    eq(ks[0].name, "escape", "a lone ESC decodes as Escape");
    eq(ks[0].meta, false, "without the meta chord");
    eq(ks[1], "EOF", "and consumes nothing after");
});

__kitS.section("ESC ESC [ Z is shift+tab under meta", () => {
    const ks = keys("\x1b\x1b[Z\x1b\x1b", 4);
    eq(ks[0].name, "tab", "ESC ESC [ Z is shift+tab under meta");
    eq(JSON.stringify([ks[0].meta, ks[0].shift]), "[true,true]", "both modifiers");
    eq(ks[1].name, "escape", "ESC ESC alone is meta+escape");
    eq(ks[1].meta, true, "meta is set");
});

__kitS.section("an unmapped CSI ~ form is an unknown key", () => {
    const ks = keys("\x1b[9~\x1b[<1~", 4);
    eq(ks[0].name, null, "an unmapped CSI ~ form is an unknown key");
    eq(ks[0].sequence, "\u001b[9~", "with its bytes intact");
    eq(ks[1].name, null, "an unexpected parameter byte marks the sequence unknown");
});
__kitS.end();

if (fails) {
    print("test_term: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_term failed");
}
print("test_term: " + n + " assertions, 0 failures");
