let _s = 0x9e3779b9;
function rnd() { _s ^= _s << 13; _s >>>= 0; _s ^= _s >>> 17; _s ^= _s << 5; _s >>>= 0; return _s; }
function rndInt(n) { return rnd() % n; }
function pick(a) { return a[rndInt(a.length)]; }

let out = [];
function emit(s) { out.push(s); }

function show(re, s, m) {
    if (m === null) return "null";
    let p = [JSON.stringify(m[0]), "@" + m.index, "li=" + re.lastIndex];
    for (let i = 1; i < m.length; i++) p.push(i + ":" + (m[i] === undefined ? "-" : JSON.stringify(m[i])));
    if (m.groups) for (const k of Object.keys(m.groups))
        p.push(k + "=" + (m.groups[k] === undefined ? "-" : JSON.stringify(m.groups[k])));
    return p.join(" ");
}

function probe(src, flags, subject) {
    let re;
    try { re = new RegExp(src, flags); }
    catch (e) { emit("COMPILE-ERR " + src + "/" + flags + ": " + e.name); return; }
    const tag = "/" + src + "/" + flags + " on " + JSON.stringify(subject.length > 40 ?
        subject.slice(0, 40) + "…(" + subject.length + ")" : subject);

    re.lastIndex = 0;
    emit(tag + " exec1  " + show(re, subject, re.exec(subject)));
    emit(tag + " exec2  " + show(re, subject, re.exec(subject)));
    re.lastIndex = 0;
    emit(tag + " test   " + re.test(subject));
    re.lastIndex = 0;
    emit(tag + " search " + subject.search(re));
    re.lastIndex = 0;
    try { emit(tag + " match  " + JSON.stringify(subject.match(re))); }
    catch (e) { emit(tag + " match  ERR " + e.name); }
    re.lastIndex = 0;
    try { emit(tag + " split  " + JSON.stringify(subject.split(re).slice(0, 12))); }
    catch (e) { emit(tag + " split  ERR " + e.name); }
    re.lastIndex = 0;
    try { emit(tag + " repl   " + JSON.stringify(subject.replace(re, (...a) => "<" + a[0] + ">").slice(0, 80))); }
    catch (e) { emit(tag + " repl   ERR " + e.name); }
    if (flags.includes("g")) {
        re.lastIndex = 0;
        let idx = [];
        try { for (const m of subject.matchAll(re)) { idx.push(m.index + ":" + m[0]); if (idx.length > 20) break; } }
        catch (e) { idx.push("ERR " + e.name); }
        emit(tag + " all    " + idx.join(","));
    }
}

const PATTERNS = [
    "x", "z", "\\.", " ",
    "needle", "NEEDLE_XYZ", "abcabc", "aaa", "ab",
    "needle\\d+", "ab[0-9]{2}", "foo(bar|baz)", "qq.*zz", "xy+z",
    "[xz]", "[aeiou]", "[0-9]", "[0-9a-f]", "[^a]", "[\\s]", "\\d\\d\\d", "\\w+",
    "^needle", "needle$", "(a|b)c", "(?=x)y", "(?!x)y", "(\\w)\\1", "\\bword\\b",
    "^.*$", "(?:)", "", "a?", "a*", "(x)?y",
    "needle", "X", "[a-f]",
    "\\u00e9", "caf\\u00e9", "\\u65e5\\u672c", "[\\u00e0-\\u00ff]", "\\u{1F600}", ".",
    "a{0,3}b", "\\d{4}-\\d{2}-\\d{2}", "a+b", "(?:ab)+c",
];
const FLAGSETS = ["", "g", "i", "gi", "u", "gu", "m", "gm", "y", "gy", "s", "d"];

const ASCII = "the quick brown fox jumps over the lazy dog 0123456789 ";
const WIDE = "naïve café — ünïcødé ✓ 日本語 ";
const SUBJECTS = [
    "", "x", "ab", "needle", "xyz",
    "short", "a".repeat(31), "a".repeat(32), "a".repeat(33),
    ASCII, ASCII.repeat(4), ASCII.repeat(40) + "NEEDLE_XYZ",
    ASCII.repeat(10) + "needle" + ASCII.repeat(10),
    "needle" + ASCII.repeat(20),
    ASCII.repeat(20) + "needle",
    WIDE, WIDE.repeat(5), WIDE.repeat(5) + "needle",
    "café ".repeat(20),
    "\u{1F600}\u{1F601}".repeat(30),
    "a\0b\0c".repeat(20),
    "ÿ".repeat(50), "Ā".repeat(50),
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab",
    "ab".repeat(500),
];

emit("=== systematic sweep ===");
for (const p of PATTERNS)
    for (const f of FLAGSETS)
        for (const s of SUBJECTS)
            probe(p, f, s);

emit("=== randomised corpus ===");
const ALPHABET = "abxyz019 .é日";
function randomSubject() {
    const n = rndInt(200);
    let s = "";
    for (let i = 0; i < n; i++) s += ALPHABET[rndInt(ALPHABET.length)];
    return s;
}
function randomPattern() {
    const atoms = ["a", "b", "x", "y", "z", "0", "1", "\\d", "\\w", "\\s", ".",
                   "[ab]", "[0-9]", "[^x]", "ab", "xy", "é", "日"];
    const quant = ["", "", "", "*", "+", "?", "{1,3}"];
    let p = "";
    const n = 1 + rndInt(4);
    for (let i = 0; i < n; i++) p += pick(atoms) + pick(quant);
    return p;
}
for (let i = 0; i < 4000; i++)
    probe(randomPattern(), pick(FLAGSETS), randomSubject());

emit("=== long-subject stress ===");
for (let i = 0; i < 600; i++) {
    const filler = pick(["ab", "the quick ", "xxxx", "éé", "0123"]);
    const reps = 20 + rndInt(300);
    const needle = pick(["needle", "z", "NEEDLE_XYZ", "éx", "999"]);
    const at = rndInt(3);
    let s = filler.repeat(reps);
    if (at === 0) s = needle + s;
    else if (at === 1) s = s + needle;
    else s = s.slice(0, s.length >> 1) + needle + s.slice(s.length >> 1);
    probe(pick(["needle", "z", "NEEDLE_XYZ", "éx", "999", "[xz]", "n[e]+dle"]),
          pick(["", "g", "i", "gi", "u"]), s);
}

console.log(out.join("\n"));
console.log("LINES " + out.length);

function eq(a, b, what) {
    if (a !== b) { console.log("SELFCHECK FAIL " + what + ": " + a + " !== " + b); throw new Error(what); }
}
{
    const big = "q".repeat(5000);
    eq(/needle/.exec(big), null, "no false positive");
    eq(/q/.exec(big).index, 0, "first position wins");
    eq(big.replace(/q/g, "").length, 0, "global replace covers all");
    const s2 = "q".repeat(1000) + "needle" + "q".repeat(1000);
    eq(/needle/.exec(s2).index, 1000, "index preserved across a skip");
    eq(s2.match(/needle/g).length, 1, "one match");
    eq(("ab".repeat(2000)).match(/ab/g).length, 2000, "every occurrence found");
    eq(("x".repeat(100) + "é").search(/é/), 100, "latin1 tail found");
    eq(("日".repeat(100) + "Z").search(/Z/), 100, "wide subject scan");
    eq(("Q".repeat(500) + "needle").search(/NEEDLE/i), 500, "ignore-case unaffected");
    const sy = /needle/y; sy.lastIndex = 5;
    eq(sy.exec("qqqqqneedle") === null, false, "sticky at lastIndex");
    eq(("abc".repeat(100)).match(/(?:)/g).length, 301, "empty matches");
    console.log("SELFCHECK ok");
}
