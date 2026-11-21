// covers: c:src/libregexp.c
// Leading-run skip in the regexp prefilter loop (libregexp re_pf_run_skip): when a pattern opens
// with a class loop (C+ or X C*) and an attempt at p fails, starts strictly inside the following
// C-run are skipped. Oracle: a sticky regexp tried at every start position, which is the
// definition of the leftmost match and never enters the prefilter loop.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_regexp_run_skip");
let fails = 0, checks = 0;
function ok(c, m) {
    checks++;
    if (!c) {
        fails++;
        print("FAIL " + m);
    }
}

function viaSticky(src, flags, s) {
    const st = new RegExp(src, flags.replace("g", "") + "y");
    const out = [];
    let i = 0;
    while (i <= s.length) {
        let j = i, m = null;
        for (; j <= s.length; j++) {
            st.lastIndex = j;
            m = st.exec(s);
            if (m) break;
        }
        if (!m) break;
        out.push(j + ":" + m.join("|"));
        i = st.lastIndex === j ? j + 1 : st.lastIndex;
    }
    return out.join(",");
}

function viaGlobal(src, flags, s) {
    const out = [];
    for (const m of s.matchAll(new RegExp(src, flags))) out.push(m.index + ":" + m.join("|"));
    return out.join(",");
}

const PATS = [
    "\\w+\\s*=",
    "\\w+?=",
    "[A-Za-z_]\\w*\\s*\\(",
    "[a-z]\\w*=",
    "[a-z]\\d*x",
    "(\\w+(?:\\s*->\\s*\\w+)*|\\*\\w+)\\s*=\\s*&\\s*(\\w+)\\s*;",
    "\\d+\\.\\d+",
    "[^\"]+\"",
    "(?:\\w+)x",
    "(\\w+)=",
    "\\w+(?<=b)=",
    "\\w+=|b",
    "\\w+=|\\*",
    "\\w+;|\\d=",
    "(\\w+)-\\1",
    "(?:\\w\\w)*=",
    "(\\w+)?x",
    "\\w*=",
    "\\w+(?!a)",
    "[ab]+c|[^ab]",
    "[ab]c?d",
    "\\s+\\{\\s*\\}",
    "\\S+=",
    "\\s+x|\\S",
    "\\s+;|\\u00a0",
    "x\\s*;",
    "\\s+;",
    "[ab]\\d*z",
];
const FLAGS = ["g", "gm", "gs", "gu", "gi"];
const ALPHA = "ab_19xZ *->=&;.(\"\n\té{}\u00a0\r\f";

let seed = 12345;
function rnd(n) {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    return seed % n;
}
function gen(len, alpha) {
    let s = "";
    for (let i = 0; i < len; i++) s += alpha[rnd(alpha.length)];
    return s;
}

const inputs = [
    "abc*de = &x; foo->bar = &baz ; qq1=  *p=&q;" + "x".repeat(8),
    "ab" + "b".repeat(40),
    "xab-b " + "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz",
    "identifier_with_long_name  = 3; another_long_identifier(" + "y".repeat(10),
    "a1b2c3d4e5f6g7h8i9j0 12.5 7.25 abc1x a99x 0.5",
    "xaabd aabbd ab12z b9z aaaaaaaaaaaaaaaaaaaaaaaaaabd",
    "   {  }      {x}  \u00a0\u00a0{\t}" + " ".repeat(40) + "{ }",
    "ab=cd" + " ".repeat(40) + "x; q\u00a0;",
    "  a ;" + "z".repeat(40) + "\t\tb\f;  a  { }",
];
let all256 = "";
for (let c = 0; c < 256; c++) all256 += String.fromCharCode(c);
inputs.push(all256, all256 + "w=" + all256, "w".repeat(31) + "=", "w".repeat(32) + "=", "w".repeat(33) + "=");
for (let k = 0; k < 120; k++) inputs.push(gen(20 + rnd(60), ALPHA));
for (let k = 0; k < 20; k++) inputs.push(gen(40, ALPHA) + "Ā" + gen(20, ALPHA));

for (const src of PATS) {
    for (const flags of FLAGS) {
        let bad = 0;
        for (const s of inputs) {
            const a = viaGlobal(src, flags, s), b = viaSticky(src, flags, s);
            if (a !== b && bad++ < 2) ok(false, "/" + src + "/" + flags + " on " + JSON.stringify(s.slice(0, 60)) + "\n  got  " + a.slice(0, 120) + "\n  want " + b.slice(0, 120));
        }
        ok(bad === 0, "/" + src + "/" + flags + " differential (" + bad + " inputs differ)");
    }
}

__kitS.section("alternative right after a run:", () => {
    const s = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab*c=&d;";
    const m = /(\w+(?:\s*->\s*\w+)*|\*\w+)\s*=\s*&\s*(\w+)\s*;/.exec(s);
    ok(m && m.index === 41 && m[1] === "*c" && m[2] === "d", "alternative right after a run: " + JSON.stringify(m));
    ok(/\w+=|b/.exec("a" + "a".repeat(40) + "b").index === 41 && /\w+=|b/.exec("ab" + "a".repeat(40)).index === 1, "overlapping second alternative is not skipped");
    ok(/(\w+)-\1/.exec("xab-b" + " ".repeat(40))?.index === 2, "backreference pattern is not skipped");
});

__kitS.section("p = &q;", () => {
    const re = /(\w+(?:\s*->\s*\w+)*|\*\w+)\s*=\s*&\s*(\w+)\s*;/g;
    re.lastIndex = 5;
    const s = "abcdefghij" + "k".repeat(30) + " p = &q;";
    const m = re.exec(s);
    ok(m && m.index === 41 && re.lastIndex === 48, "lastIndex start inside a run: " + (m && m.index) + "," + re.lastIndex);
    ok(RegExp.lastMatch === "p = &q;", "legacy lastMatch after a skipped run");
});
__kitS.end();

print("test_regexp_run_skip: " + (checks - fails) + "/" + checks + " checks");
if (fails) throw new Error(fails + " failures");
