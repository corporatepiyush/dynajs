// flags: --std
// timeout: 300
// covers: dyna:matcher.stem
// The native Porter2 (Snowball English) stemmer against the Snowball reference vocabulary: every one of the
// 42,649 voc.txt -> output.txt pairs of snowball-data/english (tests/corpus/snowball/english, BSD, listed in
// tests/test-data.tsv; tools/test-data.js fetch restores it) must match exactly. The corpus is ASCII, so a
// second check runs stem() against REF below, a direct JS transcription of the same rules that indexes by
// code point, over random words mixing a-z, y, the apostrophe and non-ASCII letters: the C code works on UTF-8
// bytes and must still count lengths and regions in characters (ied/ies, Cying, undoubling, step 1c, short
// syllables). REF itself is pinned by the corpus first, so the two agree only if both follow Snowball.
// Fixed multi-byte rows pin the character counts the random sweep may miss: "éying" (Cying, 5 characters in
// 6 bytes), "éies" (ies after one character), "éyed" (step 1c needs a letter before the preceding one).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_matcher_stem");
import * as std from "std";
import { stem } from "dyna:matcher";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d !== undefined ? "  [" + d + "]" : "")); } };

const V = (c) => c === "a" || c === "e" || c === "i" || c === "o" || c === "u" || c === "y";
const EX1 = { skis: "ski", skies: "sky", dying: "die", lying: "lie", tying: "tie", idly: "idl", gently: "gentl", ugly: "ugli", early: "earli", only: "onli",
    singly: "singl", sky: "sky", news: "news", howe: "howe", atlas: "atlas", cosmos: "cosmos", bias: "bias", andes: "andes" };
const EX2 = new Set(["inning", "outing", "canning", "herring", "earring", "proceed", "exceed", "succeed", "evening"]);
const PRE = ["gener", "commun", "arsen", "past", "univers", "later", "emerg", "organ", "inter"];
const S2 = { tional: "tion", enci: "ence", anci: "ance", abli: "able", entli: "ent", izer: "ize", ization: "ize", ational: "ate", ation: "ate", ator: "ate",
    alism: "al", aliti: "al", alli: "al", fulness: "ful", ogist: "og", ousli: "ous", ousness: "ous", iveness: "ive", iviti: "ive", biliti: "ble", bli: "ble",
    ogi: "og", fulli: "ful", lessli: "less", li: "" };
const S3 = { tional: "tion", ational: "ate", alize: "al", icate: "ic", iciti: "ic", ical: "ic", ful: "", ness: "", ative: "" };
const S4 = ["al", "ance", "ence", "er", "ic", "able", "ible", "ant", "ement", "ment", "ent", "ism", "ate", "iti", "ous", "ive", "ize", "ion"];
function REF(word) {
    if (EX1[word] !== undefined) return EX1[word];
    let w = Array.from(word);
    if (w.length <= 2) return word;
    if (w[0] === "'") w.shift();
    if (w[0] === "y") w[0] = "Y";
    for (let i = 1; i < w.length; i++) if (w[i] === "y" && V(w[i - 1])) w[i] = "Y";
    const s = () => w.join("");
    const isV = (i) => i >= 0 && i < w.length && V(w[i]);
    const ends = (x) => { const a = Array.from(x); return a.length <= w.length && a.every((c, k) => w[w.length - a.length + k] === c); };
    const longest = (list) => { let b = null; for (const x of list) if (ends(x) && (!b || x.length > b.length)) b = x; return b; };
    const cut = (n) => { w.length -= n; };
    const set = (suf, rep) => { cut(suf.length); w.push(...rep); };
    let r1 = w.length, r2 = w.length;
    const p = PRE.find((x) => s().startsWith(x));
    if (p) r1 = p.length;
    else for (let i = 1; i < w.length; i++) if (!isV(i) && isV(i - 1)) { r1 = i + 1; break; }
    for (let i = r1 + 1; i < w.length; i++) if (!isV(i) && isV(i - 1)) { r2 = i + 1; break; }
    const shortSyl = (end) => {
        const i = end - 1;
        if (end === 4 && s().startsWith("past")) return true;
        if (i === 1 && isV(0) && !isV(1)) return true;
        return i >= 2 && !isV(i - 2) && isV(i - 1) && !isV(i) && !"wxY".includes(w[i]);
    };
    let x = longest(["'", "'s", "'s'"]);
    if (x) cut(x.length);
    x = longest(["sses", "ied", "ies", "s", "us", "ss"]);
    if (x === "sses") set(x, "ss");
    else if (x === "ied" || x === "ies") set(x, w.length > 4 ? "i" : "ie");
    else if (x === "s" && w.slice(0, w.length - 2).some((c) => V(c))) cut(1);
    if (EX2.has(s())) return s();
    x = longest(["eed", "eedly", "ed", "edly", "ing", "ingly"]);
    if (x === "eed" || x === "eedly") { if (w.length - x.length >= r1) set(x, "ee"); }
    else if (x === "ing" && w.length === 5 && !isV(0) && w[1] === "y") w = [w[0], "i", "e"];
    else if (x && w.slice(0, w.length - x.length).some((c) => V(c))) {
        cut(x.length);
        const y = longest(["at", "bl", "iz", "bb", "dd", "ff", "gg", "mm", "nn", "pp", "rr", "tt"]);
        if (y === "at" || y === "bl" || y === "iz") w.push("e");
        else if (y) { if (w.length > 3) cut(1); }
        else if (r1 >= w.length && shortSyl(w.length)) w.push("e");
    }
    if (w.length > 2 && (w[w.length - 1] === "y" || w[w.length - 1] === "Y") && !isV(w.length - 2)) w[w.length - 1] = "i";
    x = longest(Object.keys(S2));
    if (x && w.length - x.length >= r1) {
        if (x === "ogi") { if (w[w.length - 4] === "l") set(x, "og"); }
        else if (x === "li") { if ("cdeghkmnrt".includes(w[w.length - 3])) cut(2); }
        else set(x, S2[x]);
    }
    x = longest(Object.keys(S3));
    if (x && w.length - x.length >= r1) {
        if (x === "ative") { if (w.length - 5 >= r2) cut(5); }
        else set(x, S3[x]);
    }
    x = longest(S4);
    if (x && w.length - x.length >= r2) {
        if (x === "ion") { if (w[w.length - 4] === "s" || w[w.length - 4] === "t") cut(3); }
        else cut(x.length);
    }
    const L = w.length;
    if (w[L - 1] === "e") { if (L - 1 >= r2 || (L - 1 >= r1 && !shortSyl(L - 1))) cut(1); }
    else if (w[L - 1] === "l" && L - 1 >= r2 && w[L - 2] === "l") cut(1);
    return s().replace(/Y/g, "y");
}

const DIR = "tests/corpus/snowball/english";
const voc = std.loadFile(DIR + "/voc.txt"), out = std.loadFile(DIR + "/output.txt");
if (voc === null || out === null) {
    print("test_matcher_stem: FAIL -- " + DIR + " is missing; run ./dynajs --std tools/test-data.js fetch");
    std.exit(1);
}
const vs = voc.split("\n"), os = out.split("\n");
if (vs[vs.length - 1] === "" && os[os.length - 1] === "") { vs.pop(); os.pop(); }
ok(vs.length === 42649 && os.length === vs.length, "the corpus holds 42,649 pairs", vs.length + "/" + os.length);
__kitS.section("want", () => {
    let bad = 0, refBad = 0;
    const t0 = Date.now();
    for (let i = 0; i < vs.length; i++) {
        const g = stem(vs[i]);
        if (g !== os[i]) { if (bad < 10) ok(false, "stem(" + JSON.stringify(vs[i]) + ")", g + " want " + os[i]); bad++; }
    }
    const ms = Date.now() - t0;
    for (let i = 0; i < vs.length; i++) if (REF(vs[i]) !== os[i]) { if (refBad < 5) ok(false, "REF(" + vs[i] + ")", REF(vs[i]) + " want " + os[i]); refBad++; }
    ok(bad === 0, "every voc.txt word stems to its output.txt line", bad + " mismatches");
    ok(refBad === 0, "the JS reference also reproduces the corpus", refBad + " mismatches");
    print("  corpus: " + vs.length + " words in " + ms + " ms");
});
__kitS.section("a 600-character word matches the reference (heap path)", () => {
    const cases = [["", ""], ["a", "a"], ["ab", "ab"], ["'s", "'s"], ["y", "y"], ["Running", "Run"], ["caresses", "caress"], ["ponies", "poni"],
        ["ties", "tie"], ["cries", "cri"], ["gas", "gas"], ["gaps", "gap"], ["kiwis", "kiwi"], ["generously", "generous"], ["'tis", "tis"],
        ["you're", "you'r"], ["sky's", "ski"], ["vying", "vie"], ["added", "add"], ["hopping", "hop"], ["pasted", "paste"], ["ogist", "ogist"],
        ["éying", "éie"], ["éies", "éie"], ["éyed", "éy"]];
    for (const [w, want] of cases) ok(stem(w) === want && REF(w) === want, "stem(" + JSON.stringify(w) + ") = " + JSON.stringify(want), stem(w));
    const long = "re" + "nationalization".repeat(40);
    ok(stem(long) === REF(long), "a 600-character word matches the reference (heap path)");
});
__kitS.section("random word", () => {
    const alpha = ["a", "e", "i", "o", "u", "y", "b", "c", "d", "g", "l", "n", "r", "s", "t", "w", "x", "'", "é", "ß", "ŷ", "т", "\u{1D4B3}"];
    const sufs = ["ies", "ied", "ying", "ing", "ed", "ly", "ness", "ation", "ogist", "s", "y", "e", "ll", "edly", "ement", "ion", "al"];
    let seed = 12345;
    const rnd = (n) => { seed = (seed * 1103515245 + 12345) >>> 0; return (seed >>> 8) % n; };
    let bad = 0;
    for (let k = 0; k < 40000; k++) {
        let w = "";
        const len = 1 + rnd(7);
        for (let j = 0; j < len; j++) w += alpha[rnd(alpha.length)];
        if (rnd(2)) w += sufs[rnd(sufs.length)];
        const a = stem(w), b = REF(w);
        if (a !== b) { if (bad < 10) ok(false, "random word " + JSON.stringify(w), a + " vs REF " + b); bad++; }
    }
    ok(bad === 0, "40,000 random words with non-ASCII letters agree with the code-point reference", bad);
});
__kitS.section("a non-string is a TypeError", () => {
    let te;
    try { stem(5); } catch (e) { te = e; }
    ok(te instanceof TypeError, "a non-string is a TypeError");
});
__kitS.end();
print(`test_matcher_stem: ${pass} passed, ${fail} failed`);
std.exit(fail ? 1 : 0);
