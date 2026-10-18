let n = 0, fails = 0;
function assert(c, msg) {
    n++;
    if (!c) { fails++; print("FAIL: " + msg); }
}
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}

const ANSI_RE = new RegExp([
    '[\\u001B\\u009B][[\\]()#;?]*(?:(?:(?:(?:;[-a-zA-Z\\d\\/#&.:=?%@~_]+)*|[a-zA-Z\\d]+(?:;[-a-zA-Z\\d\\/#&.:=?%@~_]*)*)?\\u0007)',
    '(?:(?:\\d{1,4}(?:;\\d{0,4})*)?[\\dA-PR-TZcf-nq-uy=><~]))'
].join('|'), 'g');

function stripAnsiOracle(s) { return s.replace(ANSI_RE, ''); }

const ESC = "\u001B";
const CSI_RED = ESC + "[31m", CSI_RESET = ESC + "[0m";

eq("".stripAnsi(), "", "stripAnsi empty");
eq("plain".stripAnsi(), "plain", "stripAnsi no escapes");
eq((CSI_RED + "red" + CSI_RESET).stripAnsi(), "red", "stripAnsi SGR pair");
eq((ESC + "[1;31;42mx" + ESC + "[0m").stripAnsi(), "x", "stripAnsi multi-param SGR");
eq((ESC + "]8;;https://example.com" + ESC + "\\link" + ESC + "]8;;" + ESC + "\\").stripAnsi(),
   "link", "stripAnsi OSC 8 hyperlink with ST terminator");
eq((ESC + "]0;title\u0007after").stripAnsi(), "after", "stripAnsi OSC with BEL terminator");
eq((ESC + "[2J" + ESC + "[H" + "home").stripAnsi(), "home", "stripAnsi cursor/erase");

eq((ESC).stripAnsi(), ESC, "stripAnsi lone ESC is kept");
eq(("a" + ESC).stripAnsi(), "a" + ESC, "stripAnsi trailing lone ESC is kept");

eq(("\u009B31mred" + "\u009B0m").stripAnsi(), "red", "stripAnsi 8-bit CSI");

let seed = 0x2545F491 >>> 0;
function rnd() { seed = (seed * 1664525 + 1013904223) >>> 0; return seed; }

const SEQS = [
    CSI_RED, CSI_RESET, ESC + "[0m", ESC + "[1m", ESC + "[38;5;196m",
    ESC + "[48;2;10;20;30m", ESC + "[2J", ESC + "[H", ESC + "[10;20H",
    ESC + "[K", ESC + "[?25l", ESC + "[?25h", ESC + "[1A", ESC + "[3D",
];
const TEXT = ["a", "bc", "hello", " ", "xyz", "0", "..", "word"];

let diffCases = 0;
for (let trial = 0; trial < 4000; trial++) {
    let s = "";
    const parts = 1 + (rnd() % 8);
    for (let k = 0; k < parts; k++) {
        s += (rnd() & 1) ? SEQS[rnd() % SEQS.length] : TEXT[rnd() % TEXT.length];
    }
    const want = stripAnsiOracle(s);
    const got = s.stripAnsi();
    diffCases++;
    if (got !== want) {
        assert(false, "stripAnsi differential at trial " + trial +
               ": " + JSON.stringify(s) + " -> " + JSON.stringify(got) +
               " want " + JSON.stringify(want));
        break;
    }
}
assert(diffCases === 4000, "stripAnsi differential ran all 4000 cases");

assert(stripAnsiOracle(CSI_RED + "red" + CSI_RESET) !== (CSI_RED + "red" + CSI_RESET),
       "fault injection: oracle must differ from the unstripped input");

let byteCases = 0;
for (let b = 0; b < 256; b++) {
    const ch = String.fromCharCode(b);
    for (const ctx of [ch, ESC + "[" + ch, ESC + ch, "a" + ch + "b", ESC + "[3" + ch]) {
        const got = ctx.stripAnsi();
        assert(typeof got === "string", "stripAnsi returns a string for byte " + b);
        assert(got.length <= ctx.length, "stripAnsi never grows (byte " + b + ")");
        byteCases++;
    }
}
assert(byteCases === 256 * 5, "byte sweep covered all 256 values in 5 contexts");

eq("".displayWidth(), 0, "width empty");
eq("hello".displayWidth(), 5, "width ascii");
eq("hello world".displayWidth(), 11, "width ascii with space");

eq((CSI_RED + "red" + CSI_RESET).displayWidth(), 3, "width ignores SGR");
eq((ESC + "]0;t\u0007abc").displayWidth(), 3, "width ignores OSC");

eq("\u4F60\u597D".displayWidth(), 4, "width CJK 2 chars = 4");
eq("\uFF21\uFF22".displayWidth(), 4, "width fullwidth latin = 4");
eq("\u3042".displayWidth(), 2, "width hiragana = 2");
eq("\uD55C\uAD6D".displayWidth(), 4, "width hangul syllables = 4");

eq("e\u0301".displayWidth(), 1, "width e + combining acute = 1");
eq("a\u200Bb".displayWidth(), 2, "width ZWSP = 0");
eq("\u200D".displayWidth(), 0, "width lone ZWJ = 0");

eq("\u{1f600}".displayWidth(), 2, "width grinning face = 2");
eq("\u2764\uFE0F".displayWidth(), 2, "width heart + VS16 = 2");
eq("\u{1f1fa}\u{1f1f8}".displayWidth(), 2, "width US flag (RI pair) = 2");
eq("\u{1f468}\u200D\u{1f469}\u200D\u{1f467}".displayWidth(), 2,
   "width family ZWJ sequence = 2 (one glyph, not the sum)");
eq("\u{1f44d}\u{1f3fb}".displayWidth(), 2, "width thumbs-up + skin tone = 2");

eq("a\tb".displayWidth(), 2, "width tab counts 0");
eq("a\nb".displayWidth(), 2, "width newline counts 0");

eq("\u00E9".displayWidth(), 1, "width ambiguous default = 1");
eq("\u00E9".displayWidth({ ambiguousAsWide: true }), 2, "width ambiguous opted in = 2");
eq("\u2190".displayWidth(), 1, "width leftwards arrow default = 1");
eq("\u2190".displayWidth({ ambiguousAsWide: true }), 2, "width leftwards arrow opted in = 2");

let propCases = 0;
for (let trial = 0; trial < 2000; trial++) {
    let s = "";
    const parts = 1 + (rnd() % 6);
    for (let k = 0; k < parts; k++)
        s += (rnd() & 1) ? SEQS[rnd() % SEQS.length] : TEXT[rnd() % TEXT.length];
    if (s.stripAnsi().displayWidth() !== s.displayWidth()) {
        assert(false, "width property broken on " + JSON.stringify(s));
        break;
    }
    propCases++;
}
eq(propCases, 2000, "stripAnsi(s).displayWidth() === s.displayWidth() over 2000 cases");

for (let len = 0; len < 40; len++) {
    let s = "";
    for (let i = 0; i < len; i++) s += String.fromCharCode(0x20 + (rnd() % 95));
    eq(s.displayWidth(), len, "ascii width == length at len " + len);
}

for (const len of [63, 64, 65, 127, 128, 129, 255, 256]) {
    eq("a".repeat(len).displayWidth(), len, "ascii width at len " + len);
    eq((CSI_RED + "a".repeat(len) + CSI_RESET).displayWidth(), len,
       "styled width at len " + len);
    eq((CSI_RED + "a".repeat(len) + CSI_RESET).stripAnsi(), "a".repeat(len),
       "styled strip at len " + len);
}

function g(s) { return s.graphemes(); }

eq(g("").length, 0, "graphemes empty");
eq(g("abc").join("|"), "a|b|c", "graphemes ascii");
eq(g("e\u0301").length, 1, "graphemes e + combining = 1 cluster");
eq(g("e\u0301").join(""), "e\u0301", "graphemes reassemble");
eq(g("\u{1f600}").length, 1, "graphemes astral char = 1 cluster");
eq(g("\u{1f1fa}\u{1f1f8}").length, 1, "graphemes flag = 1 cluster");
eq(g("\u{1f1fa}\u{1f1f8}\u{1f1ef}\u{1f1f5}").length, 2, "graphemes two flags = 2 clusters");
eq(g("\u{1f468}\u200D\u{1f469}\u200D\u{1f467}").length, 1, "graphemes family = 1 cluster");
eq(g("\u{1f44d}\u{1f3fb}").length, 1, "graphemes thumbs-up + tone = 1 cluster");
eq(g("a\u{1f600}b").length, 3, "graphemes mixed");

for (const s of ["", "abc", "e\u0301x", "\u{1f468}\u200D\u{1f469}a",
                 "\u{1f1fa}\u{1f1f8}!", "\u4F60\u597D", "a\u200Db"]) {
    eq(g(s).join(""), s, "graphemes partition " + JSON.stringify(s));
}

eq("hello world".wrapAnsi(20), "hello world", "wrap fits on one line");
eq("hello world".wrapAnsi(5), "hello\nworld", "wrap at the space");
eq("aaa bbb ccc".wrapAnsi(7), "aaa bbb\nccc", "wrap greedy");
eq("a b c d e".wrapAnsi(3), "a b\nc d\ne", "wrap repeatedly");

eq("abcdefgh".wrapAnsi(3), "abcdefgh", "wrap soft leaves a long word intact");
eq("abcdefgh".wrapAnsi(3, { hard: true }), "abc\ndef\ngh", "wrap hard splits it");

eq("ab\ncd".wrapAnsi(10), "ab\ncd", "wrap keeps explicit newlines");

for (const cols of [1, 2, 3, 5, 8, 13, 20]) {
    const src = "the quick brown fox jumps over the lazy dog";
    const out = src.wrapAnsi(cols, { hard: true });
    for (const line of out.split("\n"))
        assert(line.stripAnsi().displayWidth() <= cols,
               "wrap(" + cols + ") line within width: " + JSON.stringify(line));
}

{
    const styled = CSI_RED + "hello world" + CSI_RESET;
    const out = styled.wrapAnsi(5);
    eq(out.stripAnsi(), "hello\nworld", "wrap styled: text content survives");
    assert(out.split("\n")[1].indexOf(CSI_RED) === 0,
           "wrap styled: the second line re-opens the colour");
}

{
    const s = CSI_RED + "aaa " + CSI_RESET + "bbb ccc";
    const out = s.wrapAnsi(7);
    eq(out.stripAnsi(), "aaa bbb\nccc", "wrap after reset: text content");
    assert(out.split("\n")[1].indexOf(CSI_RED) === -1,
           "wrap after reset: the cleared colour is NOT re-emitted");
}

for (const bad of [0, -1]) {
    let threw = false;
    try { "x".wrapAnsi(bad); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "wrapAnsi(" + bad + ") throws RangeError");
}
{
    let threw = false;
    try { "x".wrapAnsi(); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "wrapAnsi() with no width throws TypeError");
}

if (fails) {
    print("test_string_ansi: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_string_ansi failed");
}
print("test_string_ansi: " + n + " assertions, 0 failures");
