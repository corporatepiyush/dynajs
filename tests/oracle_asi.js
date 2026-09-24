/* oracle_asi.js -- line terminators and automatic semicolon insertion.
 *
 * The lexer's whitespace handling sets `got_lf`, and `got_lf` is what decides
 * ASI. So any change to how whitespace is skipped can change the MEANING of a
 * program that still parses: `return\nx` returns undefined, `return x` returns
 * x, and neither is a syntax error.
 *
 * This hashes what such programs evaluate to, across every line-terminator
 * spelling (LF, CRLF, lone CR, and the Unicode LS/PS which are NOT handled by
 * the ASCII fast path and must still reach the switch), every restricted
 * production, and whitespace runs long enough to cross the fast path's own
 * loop several times.
 */
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
const LS = " ", PS = " ";          /* not ASCII: must reach the switch */
const NBSP = " ", BOM = "﻿";       /* whitespace, but not ASCII */
const TERMS = [LF, CRLF, CR, LS, PS];
/* runs long enough that the fast path's loop iterates many times */
const RUNS = ["", " ", "\t", "  \t ", " ".repeat(9), "\t".repeat(17),
              " \t\f\v ", "\f", "\v", " ".repeat(64)];

/* 1. the restricted productions: a line terminator changes the meaning */
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

/* 2. ASI at statement boundaries, with and without a terminator */
for (const t of TERMS) {
    for (const ws of RUNS) {
        feed("(function(){ var a = 1" + ws + t + ws + "var b = 2; return a+b; })()");
        feed("(function(){ var a = 1" + ws + t + ws + "+2; return a; })()");
        feed("(function(){ var a = 1;" + ws + t + ws + "return a; })()");
        feed("(function(){ var a = [1,2]" + ws + t + ws + "[0]; return a; })()");
        feed("(function(){ var a = 1, b = 2" + ws + t + ws + "(a=b); return a; })()");
    }
}

/* 3. the fast path must not swallow a terminator inside a template, where a
 *    raw newline is DATA rather than whitespace */
const TICK = String.fromCharCode(96);
for (const t of [LF, CRLF, CR]) {
    feed("(" + TICK + "a" + t + "b" + TICK + ").length");
    feed("(" + TICK + "a" + t + "b" + TICK + ").charCodeAt(1)");
    feed("(" + TICK + "a" + t + t + "b" + TICK + ").length");
}

/* 4. whitespace and terminators around and inside comments, which the fast
 *    path does NOT handle and must leave to the switch */
for (const t of TERMS) {
    for (const ws of RUNS) {
        feed("(function(){" + ws + "//c" + t + ws + "return 1; })()");
        feed("(function(){" + ws + "/*c" + t + "c*/" + ws + "return 2; })()");
        feed("(function(){ var a=1;" + ws + "/*" + t + "*/" + ws + "a++; return a; })()");
        feed("(function(){ return 1 /*" + t + "*/ + 1; })()");
        feed("(function(){ var a = 1 //x" + t + " + 2; return a; })()");
    }
}

/* 5. non-ASCII whitespace, which the fast path deliberately does not consume */
for (const w of [NBSP, BOM, " ", "　"]) {
    feed("(function(){" + w + "return 1; })()");
    feed("(function(){ var a = 1;" + w + "return a; })()");
    feed("(function(){ return 1 +" + w + "1; })()");
}

/* 6. leading and trailing whitespace on the whole program, and files that are
 *    nothing but whitespace */
for (const ws of RUNS)
    for (const t of TERMS) {
        feed(ws + t + "1");
        feed("1" + ws + t);
        feed(ws + t + ws + t);
    }

/* 7. long whitespace runs between every token, so the fast path is entered on
 *    each one and the token stream must be unchanged */
{
    const src = ["var", "q", "=", "1", "+", "2", "*", "3", ";", "q"];
    for (const ws of RUNS)
        for (const t of ["", LF, CRLF])
            feed("(function(){ " + src.join(ws + t + ws) + " })() ");
}

print("oracle_asi: " + cases + " cases, " + errs + " rejected, hash " +
      acc.toString(16));
