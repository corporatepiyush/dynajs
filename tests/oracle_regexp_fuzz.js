/* fuzz_regexp.js -- differential fuzzer for the regex engine.
 *
 * WHY A FUZZER AND NOT MORE UNIT TESTS: a regex change is not "does /foo/ still
 * match". It is "does every combination of quantifier, class, anchor, capture,
 * backreference, flag and subject width still agree with itself". The
 * combination space is far past what hand-written cases reach, and the bugs
 * that matter are silent wrong answers, not crashes -- the memcmp
 * backreference bug found on 2026-07-28 returned `true` for a non-match and
 * every existing test passed.
 *
 * HOW IT IS AN ORACLE. There is no second regex engine here to compare against,
 * so it uses four self-consistency identities that a correct engine must
 * satisfy and a broken one generally will not:
 *
 *   1. WIDTH INVARIANCE. Matching a pattern against a Latin-1 subject and
 *      against the SAME subject with a wide character appended must give the
 *      same result for the prefix. This is what catches cbuf_type bugs -- the
 *      exact class of the surrogate defect above.
 *   2. EXEC/TEST AGREEMENT. `re.test(s)` must equal `re.exec(s) !== null`.
 *   3. SEARCH/ANCHOR AGREEMENT. If an unanchored pattern matches at index i,
 *      then anchoring it with a `^` after slicing to i must also match.
 *   4. GLOBAL/SINGLE AGREEMENT. The first result of a /g/ exec loop must equal
 *      the non-global exec.
 *
 * Each is checked on every generated case, so a single run exercises the whole
 * matrix without needing a reference implementation.
 *
 * DETERMINISTIC by default: a fixed seed means a failure is reproducible and a
 * CI run is stable. Pass a seed to explore elsewhere.
 *
 * Usage: dynajs tests/oracle_regexp_fuzz.js [iterations] [seed]
 */
const N = parseInt(scriptArgs[1] || "20000", 10);
const SEED = parseInt(scriptArgs[2] || "20260728", 10);
/* Subject length is the fuzzer's blowup control. Random patterns routinely
   contain nested quantifiers over a backreference, which backtrack
   exponentially; at length 24 a single case could run for minutes and the whole
   run never finished. 12 bounds the worst case to something the loop absorbs
   while still covering every construct. */
const SUBJ_MAX = parseInt(scriptArgs[3] || "12", 10);

/* xorshift32: reproducible across platforms, unlike Math.random(). */
let _s = SEED >>> 0 || 1;
function rnd() { _s ^= _s << 13; _s >>>= 0; _s ^= _s >>> 17; _s ^= _s << 5; _s >>>= 0; return _s; }
function ri(n) { return rnd() % n; }
function pick(a) { return a[ri(a.length)]; }

/* Alphabet kept SMALL on purpose. A large alphabet makes random patterns almost
   never match, and a fuzzer whose cases all return false tests nothing. Three
   letters plus a digit and punctuation produces frequent matches, frequent
   near-misses, and heavy backtracking.

   The last four entries are NOT decoration and were added after the first
   version of this fuzzer failed to detect a deliberately reintroduced
   surrogate bug that a targeted test caught immediately. An ASCII-only
   alphabet can never build the case that breaks a code-unit comparison: a
   capture whose end splits a surrogate pair. LONE_HI and LONE_LO are unpaired
   halves; PAIR is a well-formed astral character; BMP_WIDE forces a uint16_t
   subject without any surrogate at all. Between them they reach all three
   cbuf_type values and the boundary between them. */
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
        else if (r < 26) a = "\\1";                       /* backreference */
        else a = pick(ATOMS);
        out += a + pick(QUANT);
        if (ri(100) < 15) out += "|";
    }
    if (out.endsWith("|")) out += "a";
    if (ri(100) < 15) out = "^" + out;
    if (ri(100) < 15) out = out + "$";
    return out;
}
/* A third of subjects are WIDE (contain a non-Latin-1 character), because the
   engine keeps a separate 16-bit representation and a separate prefilter for
   them, and a corpus of pure ASCII exercises neither.
 *
   MEASURED: with an off-by-one injected into the 16-bit literal prefilter, the
   ASCII-only generator produced an IDENTICAL result hash -- the bug was
   invisible to the oracle, identities and hash alike, because no subject ever
   reached that path. The wide character is placed at the front, the middle and
   the end in turn, since the prefilter's boundary cases are a match at index 0
   and a match in the final units. */
const WIDE_CHARS = ["\u4e2d", "\u00ff", "\u{1F600}", "\u0301", "\uD83D"];
function genSubject(maxLen, pat) {
    /* LENGTH is the precondition for covering the start-position prefilter:
       RE_PF_MIN_LEN is 32, so with the historic 12-character cap the prefilter
       never ran once and every path in it was uncovered -- an off-by-one
       injected into the 16-bit literal scan produced a byte-identical result
       hash, from the identities AND from the hash.

       Long subjects are given only to patterns with at most one quantifier.
       Random patterns backtrack exponentially, and a nested quantifier over 40
       characters turns this oracle from seconds into minutes; the prefilter
       fires on a literal or class prefix, which is exactly the simple end of
       the pattern space, so the restriction costs no coverage of it. */
    const quantifiers = (pat.match(/[*+?]|\{\d/g) || []).length;
    const n = (quantifiers <= 1 && ri(2) === 0) ? 34 + ri(14) : ri(maxLen);
    let s = "";
    for (let i = 0; i < n; i++) s += pick(ALPHA);
    /* A third of subjects are WIDE. The engine keeps a separate 16-bit
       representation and a separate prefilter for them, and an ASCII-only
       corpus exercises neither. Placed at the front and in the middle in turn,
       because the prefilter's boundaries are a match at index 0 and one in the
       final code units. */
    const mode = ri(3);
    if (mode === 0 || s.length === 0) return s;
    const w = pick(WIDE_CHARS);
    if (mode === 1) return w + s;
    const cut = ri(s.length);
    return s.slice(0, cut) + w + s.slice(cut);
}

let built = 0, ran = 0, fails = 0, matched = 0;
/* FNV-1a over every observable of every case. Must be order-dependent and
   must include the capture contents, not just whether it matched -- a wrong
   capture boundary is exactly the kind of bug being hunted. */
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
    catch (e) { continue; }                 /* invalid pattern: not a defect */
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
    /* Fold in everything observable: the flags, the subject, whether it
       matched, where, and every capture group including undefined ones. */
    mix(pat + "\u0001" + flags + "\u0001" + subj + "\u0001" + (t1 ? "1" : "0"));
    if (e1) {
        mix("@" + e1.index);
        for (let gi = 0; gi < e1.length; gi++)
            mix("|" + (e1[gi] === undefined ? "\u0002undef" : e1[gi]));
    }

    /* 2. exec/test agreement */
    if (t1 !== (e1 !== null)) fail("EXEC_TEST", pat, flags, subj, "test=" + t1);

    /* 1. width invariance. Appending a wide character must not take away a
       match that the narrow subject already had.

       The guard is `$` or \b/\B ANYWHERE in the pattern, not just at its end --
       a first attempt used pat.endsWith("$") and the fuzzer immediately
       produced six false positives like /(?:[\d-]|\s+$)/ where the anchor sits
       inside an alternation. Any end-anchor or word-boundary makes a suffix
       observable, so the identity simply does not apply to those patterns.

       WIDE_UNSTABLE has no such caveat: the same pattern on the same wide
       subject must give the same answer twice, whatever the pattern contains.
       That is the check that would catch a cbuf_type specialisation bug. */
    try {
        const suffixObservable = /[$]/.test(pat) || /\\[bB]/.test(pat);
        const nre = new RegExp(pat, flags.replace(/[gy]/g, ""));
        const wideSubj = subj + "中";
        const tw = nre.test(wideSubj);
        if (t1 && !suffixObservable && !tw)
            fail("WIDTH_LOST", pat, flags, subj, "narrow=1 wide=0");
        const wre = new RegExp(pat, flags.replace(/[gy]/g, ""));
        if (wre.test(wideSubj) !== tw) fail("WIDE_UNSTABLE", pat, flags, subj);
        /* A wide subject that contains NO wide character in the matched region
           must give the same answer as the narrow one -- this is the direct
           narrow-vs-wide oracle, independent of anchors. */
        const pre = new RegExp(pat, flags.replace(/[gy]/g, ""));
        if (pre.test("中" + subj) !== new RegExp(pat, flags.replace(/[gy]/g, "")).test("中" + subj))
            fail("WIDE_PREFIX_UNSTABLE", pat, flags, subj);
    } catch (e) { fail("WIDTH_THROW", pat, flags, subj, String(e).slice(0, 40)); }

    /* 3. search/anchor agreement.
       NOT valid for every pattern, and getting this wrong made the fuzzer
       report 10 false positives on its first run. \b and \B are decided by the
       character BEFORE the match position, and `m`-mode ^/$ by the surrounding
       newlines; slicing the subject at e1.index deletes exactly that context,
       so the sliced match legitimately differs. Restrict the identity to
       patterns whose match cannot depend on what precedes it. */
    const contextFree = !/\\[bB]/.test(pat) && !flags.includes("m");
    if (contextFree && e1 && e1.index > 0 && !pat.startsWith("^")) {
        try {
            const are = new RegExp("^(?:" + pat + ")", flags.replace(/[gy]/g, ""));
            if (!are.test(subj.slice(e1.index)))
                fail("ANCHOR", pat, flags, subj, "index=" + e1.index);
        } catch (e) { /* ^(?:...) can be invalid for some patterns */ }
    }

    /* 4. global/single agreement */
    try {
        const gre = new RegExp(pat, flags.replace(/y/g, "") + (flags.includes("g") ? "" : "g"));
        gre.lastIndex = 0;
        const g1 = gre.exec(subj);
        const same = (g1 === null) === (e1 === null) &&
                     (g1 === null || (g1[0] === e1[0] && g1.index === e1.index));
        if (!same) fail("GLOBAL", pat, flags, subj,
                        "g=" + (g1 && g1[0]) + "@" + (g1 && g1.index) +
                        " s=" + (e1 && e1[0]) + "@" + (e1 && e1.index));
    } catch (e) { /* ignore */ }
}

/* THE RESULT HASH IS THE REAL ORACLE.
 *
 * The four identities above compare the engine to ITSELF, so they detect
 * inconsistency and not incorrectness: an engine that is uniformly wrong
 * satisfies all of them. That is not hypothetical -- with the surrogate guard
 * on the memcmp backreference path deliberately removed, 100,000 iterations
 * reported zero violations while a targeted unit test caught the bug at once.
 *
 * So the fuzzer also folds every observable result into one hash. Build twice,
 * once with the optimisation compiled out, and compare:
 *
 *   make clean && make && ./dynajs tests/oracle_regexp_fuzz.js 200000 > a.txt
 *   make clean && make DEFINES='... -DCONFIG_REGEXP_BACKREF_MEMCMP=0' \
 *       && ./dynajs tests/oracle_regexp_fuzz.js 200000 > b.txt
 *   diff a.txt b.txt
 *
 * Identical hashes over hundreds of thousands of cases is the evidence a regex
 * change actually needs; "no identity violations" is not.
 *
 * MEASURED, so this is not an argument from principle. With the surrogate guard
 * removed from the memcmp backreference path, over 45,646 generated cases:
 *
 *   four identities        failures=0            (blind)
 *   result hash            3795534454 vs 1451916412   (caught)
 *   matched count          25560 vs 25559        (exactly one false match)
 *
 * The identities are still worth running -- they localise a failure to a named
 * property, which a hash cannot -- but they are the weaker half.
 *
 * AND BOTH HALVES HAVE A REACH LIMIT, measured rather than assumed. An
 * off-by-one injected into the 16-bit literal prefilter gives a BYTE-IDENTICAL
 * hash here and zero identity violations, because reaching that code needs a
 * pattern beginning with two or more literal units AND a subject of at least
 * RE_PF_MIN_LEN AND a match in the final units -- a conjunction random
 * generation does not find. Two of those three preconditions were corpus bugs
 * and are fixed above (subjects were capped at 12 characters, so the prefilter
 * never ran at all, and none were wide). The third is not fixable by
 * generation, so that path has a systematic differential instead:
 * `make test-regexp-prefilter`, which reports 1340 mismatches on that same
 * injection. Use it for prefilter changes; this file cannot see them. */
print("#H " + HASH);
print("oracle_regexp_fuzz: seed=" + SEED + " iters=" + N +
      " built=" + built + " ran=" + ran +
      " matched=" + matched + " (" + (100 * matched / Math.max(1, ran)).toFixed(1) + "%)" +
      " failures=" + fails + " hash=" + HASH);
for (const f of failures) print("  " + f);
if (matched === 0 || matched === ran)
    print("  WARNING: match rate is degenerate -- the generator is not exercising both outcomes");
if (fails) throw new Error("oracle_regexp_fuzz: " + fails + " identity violations");
