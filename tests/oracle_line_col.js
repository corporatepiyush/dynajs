function fnv(s) {
    let h = 2166136261 >>> 0;
    for (let i = 0; i < s.length; i++) {
        h ^= s.charCodeAt(i) & 0xff;
        h = Math.imul(h, 16777619) >>> 0;
        h ^= (s.charCodeAt(i) >>> 8);
        h = Math.imul(h, 16777619) >>> 0;
    }
    return h >>> 0;
}
let acc = 0, cases = 0, withPos = 0;

function feed(src) {
    cases++;
    let out;
    try {
        eval(src);
        out = "OK";
    } catch (e) {
        out = String(e.stack || (e.name + ":" + e.message));
        const m = out.match(/:(\d+):(\d+)/);
        out = m ? ("POS" + m[1] + ":" + m[2]) : ("MSG" + e.name);
        if (m) withPos++;
    }
    acc = (Math.imul(acc, 31) + fnv(out)) >>> 0;
}

const NL = "\n";

for (let line = 0; line < 12; line++) {
    for (let col = 0; col < 12; col++) {
        let src = "";
        for (let l = 0; l < line; l++) src += "var l" + l + " = 1;" + NL;
        src += " ".repeat(col) + "@";
        feed(src);
    }
}

const WIDE = ["é", "€", "😀", "日本語"];
for (const w of WIDE) {
    for (let n = 0; n <= 6; n++) {
        feed('var s = "' + w.repeat(n) + '"; @');
        feed('var s = "' + w.repeat(n) + '";' + NL + "@");
        feed("// " + w.repeat(n) + NL + "@");
    }
}

for (const sep of [NL, "\r" + NL, "\r", NL + NL, NL + "\r" + NL]) {
    feed("var a = 1;" + sep + "@");
    feed("var a = 1;" + sep + "var b = 2;" + sep + "@");
}

feed("@");
feed(NL.repeat(20) + "@");
feed("x".repeat(500) + NL + "@");
feed("var a = 1;".repeat(200) + "@");
feed(NL.repeat(200));

for (let depth = 1; depth <= 8; depth++) {
    let src = "";
    for (let d = 0; d < depth; d++)
        src += "function f" + d + "(){" + NL + "  var v" + d + " = " + d + ";" + NL;
    src += "  @" + NL;
    for (let d = 0; d < depth; d++) src += "}" + NL;
    feed(src);
}

for (let n of [10, 50, 200, 500]) {
    let src = "";
    for (let i = 0; i < n; i++) src += "function g" + i + "(a,b){ return a+b; }" + NL;
    src += "@" + NL;
    feed(src);
    let src2 = "";
    for (let i = 0; i < n; i++) src2 += "/* é" + i + " */ function h" + i + "(){}" + NL;
    src2 += "  @" + NL;
    feed(src2);
}

// ============================================================================
// PINNED VERDICT (B1-13). This file used to accumulate a hash and PRINT it.
// Nothing compared it against anything, so the only way it could ever fail was
// a crash: six core-stage suites read as parser coverage while asserting
// nothing about the parser. The pin below is the differential this suite was
// written for, made load-bearing inside a one-binary gate -- same generator,
// same case list, and any change in line/column changes the hash and turns the
// stage red.
//
// Provenance: captured from the release build at workspace base (branch
// audit-hardening). To move a pin, run this file's sibling tool
// tests/pin_oracle_verdicts.py and REVIEW the new line: a pin that moves
// without a reviewed behaviour change is a deleted test.
const PIN_CASES = 259;
const PIN_COUNT = 258;
const PIN_HASH = "7a0bd105";
const gotCount = withPos;
const gotHash = acc.toString(16);
let pinBad = 0;
if (cases !== PIN_CASES) {
    print("FAIL: oracle_line_col fed " + cases + " cases, the pinned generator feeds " + PIN_CASES);
    pinBad++;
}
if (gotCount !== PIN_COUNT) {
    print("FAIL: oracle_line_col " + gotCount + " positions, pinned " + PIN_COUNT);
    pinBad++;
}
if (gotHash !== PIN_HASH) {
    print("FAIL: oracle_line_col hash " + gotHash + ", pinned " + PIN_HASH + " -- parser behaviour changed");
    pinBad++;
}
if (pinBad === 0)
    print("oracle_line_col: OK (" + cases + " cases, " + gotCount + " positions, hash " + gotHash + " matches the pin)");
if (pinBad !== 0)
    throw new Error("oracle_line_col: " + pinBad + " pinned verdict(s) changed");

