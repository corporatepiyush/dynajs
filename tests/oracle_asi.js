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

const LF = "\n", CR = "\r", CRLF = "\r\n";
const LS = " ", PS = " ";
const NBSP = " ", BOM = "﻿";
const TERMS = [LF, CRLF, CR, LS, PS];
const RUNS = ["", " ", "\t", "  \t ", " ".repeat(9), "\t".repeat(17),
              " \t\f\v ", "\f", "\v", " ".repeat(64)];

for (const t of TERMS) {
    for (const ws of RUNS) {
        feed("(function(){ return" + ws + t + ws + "1; })()");
        feed("(function(){ var a = 1, b = 1;" + ws + "a" + ws + t + ws + "++b; return [a,b].join(); })()");
        feed("(function(){ var a = 1, b = 1; a" + ws + t + ws + "--b; return [a,b].join(); })()");
        feed("(function*(){ yield" + ws + t + ws + "1; })().next().value");
        feed("(function(){ try { throw" + ws + t + ws + "1; } catch(e) { return 'caught' + e; } })()");
        feed("(function(){ x: for(;;){ break" + ws + t + ws + "x; } return 'b'; })()");
        feed("(function(){ var n=0; x: for(var i=0;i<2;i++){ n++; continue" + ws + t + ws + "x; } return n; })()");
        feed("(async function(){ return 1; })" + ws + t + ws + "()");
    }
}

for (const t of TERMS) {
    for (const ws of RUNS) {
        feed("(function(){ var a = 1" + ws + t + ws + "var b = 2; return a+b; })()");
        feed("(function(){ var a = 1" + ws + t + ws + "+2; return a; })()");
        feed("(function(){ var a = 1;" + ws + t + ws + "return a; })()");
        feed("(function(){ var a = [1,2]" + ws + t + ws + "[0]; return a; })()");
        feed("(function(){ var a = 1, b = 2" + ws + t + ws + "(a=b); return a; })()");
    }
}

const TICK = String.fromCharCode(96);
for (const t of [LF, CRLF, CR]) {
    feed("(" + TICK + "a" + t + "b" + TICK + ").length");
    feed("(" + TICK + "a" + t + "b" + TICK + ").charCodeAt(1)");
    feed("(" + TICK + "a" + t + t + "b" + TICK + ").length");
}

for (const t of TERMS) {
    for (const ws of RUNS) {
        feed("(function(){" + ws + "//c" + t + ws + "return 1; })()");
        feed("(function(){" + ws + "/*c" + t + "c*/" + ws + "return 2; })()");
        feed("(function(){ var a=1;" + ws + "/*" + t + "*/" + ws + "a++; return a; })()");
        feed("(function(){ return 1 /*" + t + "*/ + 1; })()");
        feed("(function(){ var a = 1 //x" + t + " + 2; return a; })()");
    }
}

for (const w of [NBSP, BOM, " ", "　"]) {
    feed("(function(){" + w + "return 1; })()");
    feed("(function(){ var a = 1;" + w + "return a; })()");
    feed("(function(){ return 1 +" + w + "1; })()");
}

for (const ws of RUNS)
    for (const t of TERMS) {
        feed(ws + t + "1");
        feed("1" + ws + t);
        feed(ws + t + ws + t);
    }

{
    const src = ["var", "q", "=", "1", "+", "2", "*", "3", ";", "q"];
    for (const ws of RUNS)
        for (const t of ["", LF, CRLF])
            feed("(function(){ " + src.join(ws + t + ws) + " })() ");
}

// ============================================================================
// PINNED VERDICT (B1-13). This file used to accumulate a hash and PRINT it.
// Nothing compared it against anything, so the only way it could ever fail was
// a crash: six core-stage suites read as parser coverage while asserting
// nothing about the parser. The pin below is the differential this suite was
// written for, made load-bearing inside a one-binary gate -- same generator,
// same case list, and any change in ASI changes the hash and turns the
// stage red.
//
// Provenance: captured from the release build at workspace base (branch
// audit-hardening). To move a pin, run this file's sibling tool
// tests/pin_oracle_verdicts.py and REVIEW the new line: a pin that moves
// without a reviewed behaviour change is a deleted test.
const PIN_CASES = 1101;
const PIN_COUNT = 101;
const PIN_HASH = "7df80d83";
const gotCount = errs;
const gotHash = acc.toString(16);
let pinBad = 0;
if (cases !== PIN_CASES) {
    print("FAIL: oracle_asi fed " + cases + " cases, the pinned generator feeds " + PIN_CASES);
    pinBad++;
}
if (gotCount !== PIN_COUNT) {
    print("FAIL: oracle_asi " + gotCount + " rejected, pinned " + PIN_COUNT);
    pinBad++;
}
if (gotHash !== PIN_HASH) {
    print("FAIL: oracle_asi hash " + gotHash + ", pinned " + PIN_HASH + " -- parser behaviour changed");
    pinBad++;
}
if (pinBad === 0)
    print("oracle_asi: OK (" + cases + " cases, " + gotCount + " rejected, hash " + gotHash + " matches the pin)");
if (pinBad !== 0)
    throw new Error("oracle_asi: " + pinBad + " pinned verdict(s) changed");

