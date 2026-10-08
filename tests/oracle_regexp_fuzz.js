const N = parseInt(scriptArgs[1] || "20000", 10);
const SEED = parseInt(scriptArgs[2] || "20260728", 10);
const SUBJ_MAX = parseInt(scriptArgs[3] || "12", 10);

let _s = SEED >>> 0 || 1;
function rnd() { _s ^= _s << 13; _s >>>= 0; _s ^= _s >>> 17; _s ^= _s << 5; _s >>>= 0; return _s; }
function ri(n) { return rnd() % n; }
function pick(a) { return a[ri(a.length)]; }

const LONE_HI = "\uD83D", LONE_LO = "\uDE00", PAIR = "😀", BMP_WIDE = "中";
const ALPHA = ["a", "b", "c", "1", "-", " ",
               LONE_HI, LONE_LO, PAIR, BMP_WIDE];
const ATOMS = ["a", "b", "c", "1", ".", "\\d", "\\w", "\\s", "\\D", "\\W", "\\S",
               "[abc]", "[^abc]", "[a-c1-3]", "[\\d-]", "(?:ab)", "(a)", "(b|c)",
               "(?=a)", "(?!a)", "\\b", "\\B"];
const QUANT = ["", "", "", "*", "+", "?", "{2}", "{1,3}", "{0,2}", "*?", "+?", "??", "{1,}"];
const FLAGSETS = ["", "i", "m", "s", "u", "im", "is", "ms", "gi", "g", "y", "su"];

function genPattern(depth) {
    const n = 1 + ri(4);
    let out = "";
    for (let i = 0; i < n; i++) {
        let a;
        const r = ri(100);
        if (r < 12 && depth < 2) a = "(" + genPattern(depth + 1) + ")";
        else if (r < 20 && depth < 2) a = "(?:" + genPattern(depth + 1) + ")";
        else if (r < 26) a = "\\1";
        else a = pick(ATOMS);
        out += a + pick(QUANT);
        if (ri(100) < 15) out += "|";
    }
    if (out.endsWith("|")) out += "a";
    if (ri(100) < 15) out = "^" + out;
    if (ri(100) < 15) out = out + "$";
    return out;
}
const WIDE_CHARS = ["\u4e2d", "\u00ff", "\u{1F600}", "\u0301", "\uD83D"];
function genSubject(maxLen, pat) {
    const quantifiers = (pat.match(/[*+?]|\{\d/g) || []).length;
    const n = (quantifiers <= 1 && ri(2) === 0) ? 34 + ri(14) : ri(maxLen);
    let s = "";
    for (let i = 0; i < n; i++) s += pick(ALPHA);
    const mode = ri(3);
    if (mode === 0 || s.length === 0) return s;
    const w = pick(WIDE_CHARS);
    if (mode === 1) return w + s;
    const cut = ri(s.length);
    return s.slice(0, cut) + w + s.slice(cut);
}

let built = 0, ran = 0, fails = 0, matched = 0;
let HASH = 2166136261 >>> 0;
function mix(str) {
    for (let i = 0; i < str.length; i++) {
        HASH ^= str.charCodeAt(i);
        HASH = Math.imul(HASH, 16777619) >>> 0;
    }
    HASH ^= 10; HASH = Math.imul(HASH, 16777619) >>> 0;
}
const failures = [];
function fail(kind, pat, flags, subj, extra) {
    fails++;
    if (failures.length < 20)
        failures.push(kind + "  /" + pat + "/" + flags + "  subj=" + JSON.stringify(subj) +
                      (extra ? "  " + extra : ""));
}

for (let iter = 0; iter < N; iter++) {
    const pat = genPattern(0);
    const flags = pick(FLAGSETS);
    let re;
    try { re = new RegExp(pat, flags); }
    catch (e) { continue; }
    built++;
    const subj = genSubject(SUBJ_MAX, pat);

    let t1, e1;
    try {
        const nre = new RegExp(pat, flags.replace(/[gy]/g, ""));
        t1 = nre.test(subj);
        nre.lastIndex = 0;
        e1 = nre.exec(subj);
    } catch (e) { continue; }
    ran++;
    if (t1) matched++;
    mix(pat + "\u0001" + flags + "\u0001" + subj + "\u0001" + (t1 ? "1" : "0"));
    if (e1) {
        mix("@" + e1.index);
        for (let gi = 0; gi < e1.length; gi++)
            mix("|" + (e1[gi] === undefined ? "\u0002undef" : e1[gi]));
    }

    if (t1 !== (e1 !== null)) fail("EXEC_TEST", pat, flags, subj, "test=" + t1);

    try {
        const suffixObservable = /[$]/.test(pat) || /\\[bB]/.test(pat);
        const nre = new RegExp(pat, flags.replace(/[gy]/g, ""));
        const wideSubj = subj + "中";
        const tw = nre.test(wideSubj);
        if (t1 && !suffixObservable && !tw)
            fail("WIDTH_LOST", pat, flags, subj, "narrow=1 wide=0");
        const wre = new RegExp(pat, flags.replace(/[gy]/g, ""));
        if (wre.test(wideSubj) !== tw) fail("WIDE_UNSTABLE", pat, flags, subj);
        const pre = new RegExp(pat, flags.replace(/[gy]/g, ""));
        if (pre.test("中" + subj) !== new RegExp(pat, flags.replace(/[gy]/g, "")).test("中" + subj))
            fail("WIDE_PREFIX_UNSTABLE", pat, flags, subj);
    } catch (e) { fail("WIDTH_THROW", pat, flags, subj, String(e).slice(0, 40)); }

    const contextFree = !/\\[bB]/.test(pat) && !flags.includes("m");
    if (contextFree && e1 && e1.index > 0 && !pat.startsWith("^")) {
        try {
            const are = new RegExp("^(?:" + pat + ")", flags.replace(/[gy]/g, ""));
            if (!are.test(subj.slice(e1.index)))
                fail("ANCHOR", pat, flags, subj, "index=" + e1.index);
        } catch (e) {  }
    }

    try {
        const gre = new RegExp(pat, flags.replace(/y/g, "") + (flags.includes("g") ? "" : "g"));
        gre.lastIndex = 0;
        const g1 = gre.exec(subj);
        const same = (g1 === null) === (e1 === null) &&
                     (g1 === null || (g1[0] === e1[0] && g1.index === e1.index));
        if (!same) fail("GLOBAL", pat, flags, subj,
                        "g=" + (g1 && g1[0]) + "@" + (g1 && g1.index) +
                        " s=" + (e1 && e1[0]) + "@" + (e1 && e1.index));
    } catch (e) {  }
}

print("#H " + HASH);
print("oracle_regexp_fuzz: seed=" + SEED + " iters=" + N +
      " built=" + built + " ran=" + ran +
      " matched=" + matched + " (" + (100 * matched / Math.max(1, ran)).toFixed(1) + "%)" +
      " failures=" + fails + " hash=" + HASH);
for (const f of failures) print("  " + f);
if (matched === 0 || matched === ran)
    print("  WARNING: match rate is degenerate -- the generator is not exercising both outcomes");
if (fails) throw new Error("oracle_regexp_fuzz: " + fails + " identity violations");
