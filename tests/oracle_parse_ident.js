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
let acc = 0, cases = 0, errs = 0;
function feed(src) {
    cases++;
    let out;
    try { out = "V" + String(eval(src)); }
    catch (e) { errs++; out = "E" + e.name; }
    acc = (Math.imul(acc, 31) + fnv(out)) >>> 0;
}

for (let b = 0; b < 128; b++) {
    const ch = String.fromCharCode(b);
    feed("(function(){ var a" + ch + " = 1; return typeof a" + ch + "; })()");
    feed("(function(){ var a" + ch + "z = 1; return a" + ch + "z; })()");
}

for (let len = 1; len <= 300; len++) {
    const name = "v" + "x".repeat(len - 1);
    feed("(function(){ var " + name + " = " + (len % 97) + "; return " + name + "; })()");
}
for (const len of [400, 511, 512, 513, 1000, 4096]) {
    const name = "w" + "y".repeat(len);
    feed("(function(){ var " + name + " = 7; return " + name + "; })()");
}

const MIX = ["café", "naïve", "Ünï", "日本語", "emoji", "$dollar", "_under",
             "a$b_c0", "$", "_", "$$", "__", "a1234567890"];
for (const nm of MIX)
    feed("(function(){ var " + nm + " = 1; return " + nm + "; })()");

const BS = String.fromCharCode(92);
for (const nm of ["a" + BS + "u0062c", BS + "u0061bc", "ab" + BS + "u0063",
                  BS + "u{62}x", "a" + BS + "u{0063}"])
    feed("(function(){ var " + nm + " = 5; return " + nm + "; })()");

for (let len = 1; len <= 140; len++) {
    const nm = "p".repeat(len);
    feed("(function(){ class C { #" + nm + " = 3; get(){ return this.#" + nm +
         "; } } return new C().get(); })()");
}

for (const kw of ["if", "ifx", "let", "lets", "await", "awaited", "yield",
                  "yields", "class", "classy", "of", "off", "get", "getter",
                  "static", "statically", "constructor", "prototype"])
    feed("(function(){ var " + kw + "_ = 1; return typeof " + kw + "_; })()");

for (let i = 0; i < 100; i++) {
    let src = "(function(){ ";
    for (let j = 0; j < 20; j++) src += "var id_" + i + "_" + j + " = " + j + "; ";
    src += "return id_" + i + "_19; })()";
    feed(src);
}

// ============================================================================
// PINNED VERDICT (B1-13). This file used to accumulate a hash and PRINT it.
// Nothing compared it against anything, so the only way it could ever fail was
// a crash: six core-stage suites read as parser coverage while asserting
// nothing about the parser. The pin below is the differential this suite was
// written for, made load-bearing inside a one-binary gate -- same generator,
// same case list, and any change in identifier lexing changes the hash and turns the
// stage red.
//
// Provenance: captured from the release build at workspace base (branch
// audit-hardening). To move a pin, run this file's sibling tool
// tests/pin_oracle_verdicts.py and REVIEW the new line: a pin that moves
// without a reviewed behaviour change is a deleted test.
const PIN_CASES = 838;
const PIN_COUNT = 117;
const PIN_HASH = "ae55652a";
const gotCount = errs;
const gotHash = acc.toString(16);
let pinBad = 0;
if (cases !== PIN_CASES) {
    print("FAIL: oracle_parse_ident fed " + cases + " cases, the pinned generator feeds " + PIN_CASES);
    pinBad++;
}
if (gotCount !== PIN_COUNT) {
    print("FAIL: oracle_parse_ident " + gotCount + " rejected, pinned " + PIN_COUNT);
    pinBad++;
}
if (gotHash !== PIN_HASH) {
    print("FAIL: oracle_parse_ident hash " + gotHash + ", pinned " + PIN_HASH + " -- parser behaviour changed");
    pinBad++;
}
if (pinBad === 0)
    print("oracle_parse_ident: OK (" + cases + " cases, " + gotCount + " rejected, hash " + gotHash + " matches the pin)");
if (pinBad !== 0)
    throw new Error("oracle_parse_ident: " + pinBad + " pinned verdict(s) changed");

