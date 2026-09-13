/* bench_regexp.js -- the measurement that has to exist before any regex change.
 *
 * RUN THIS BEFORE AND AFTER ANY CHANGE TO src/libregexp.c.
 *
 * The engine is Bellard's backtracking VM. Every proposed optimisation to it
 * (cloning the backtracker per cbuf_type, computed-goto dispatch, fusing
 * REOP_char runs, memcmp backreferences, range bitmaps, removing the
 * compile-time dbuf_insert memmoves) targets a DIFFERENT part of it, and the
 * only way to tell a real win from a plausible story is to have all the parts
 * measured separately beforehand.
 *
 * Deliberate structure:
 *
 *  - EXEC and COMPILE are timed separately. Several proposals are compile-time
 *    only (the O(n^2) memmoves); timing them together hides both.
 *  - Every exec case is run on a NARROW (Latin-1) and a WIDE (UTF-16) subject.
 *    cbuf_type is exactly what the biggest proposal specialises on, and this
 *    engine's prefilter already gives up on 16-bit, so the two must never be
 *    averaged into one number.
 *  - MATCH and NO-MATCH are separate rows. A prefilter or a literal-fusion
 *    change helps the scanning case and can do nothing for the anchored one;
 *    reporting only one of them is how a bypass gets credited with a win it
 *    does not have.
 *  - Pathological/backtracking cases are included and kept forever. A change
 *    that speeds the happy path while worsening catastrophic backtracking is
 *    not an improvement.
 *
 * Output lines beginning with `#R` are machine-readable:
 *   #R <group> <name> <subject-width> <ms> <ops/s>
 *
 * Usage: dynajs tests/bench_regexp.js [scale]
 */
const SCALE = parseFloat(scriptArgs[1] || "1");

/* ---- subjects -------------------------------------------------------- */
/* Narrow: pure Latin-1, so JSString stays 8-bit. Wide: contains one non-Latin-1
 * character, which forces the whole string to uint16_t. Same length and same
 * match positions, so the two rows are comparable. */
function mkNarrow(n) {
    let parts = [];
    for (let i = 0; i < n; i++) parts.push("lorem ipsum dolor sit amet " + (i % 997));
    return parts.join(" | ");
}
function mkWide(n) { return mkNarrow(n) + "中"; }

const SUBJ_N = mkNarrow(400);          /* ~13 KB */
const SUBJ_W = mkWide(400);
const NEEDLE_LATE_N = SUBJ_N + " ZZTARGETZZ";
const NEEDLE_LATE_W = SUBJ_W + " ZZTARGETZZ";

function ms(f, reps) {
    f(Math.max(1, reps / 20 | 0));                     /* warm */
    const t0 = performance.now();
    f(reps);
    return performance.now() - t0;
}
function row(group, name, width, reps, f) {
    const t = ms(f, reps);
    print("#R " + group + " " + name + " " + width + " " + t.toFixed(3) + " " +
          (reps / (t / 1000)).toFixed(0));
    return t;
}

/* ---- 1. literal patterns (REOP_char runs; the fusion proposal) --------- */
/* Short and long literals, matching early and not at all, on both widths. */
{
    const pats = [["short", /dolor/g], ["long", /lorem ipsum dolor sit amet/g],
                  ["miss", /ZZNOTHEREZZ/g]];
    for (const [nm, re] of pats)
        for (const [w, s] of [["narrow", SUBJ_N], ["wide", SUBJ_W]])
            row("literal", nm, w, 150000 * SCALE, n => {
                for (let i = 0; i < n; i++) { re.lastIndex = 0; re.test(s); }
            });
}

/* ---- 2. start-position scanning (the prefilter) ----------------------- */
/* A needle only at the very end forces the engine to scan the whole subject.
 * This is where the SIMD prefilter pays, and where it currently gives up on
 * 16-bit -- the gap the teardown's 4.2 proposes to close. */
{
    for (const [w, s] of [["narrow", NEEDLE_LATE_N], ["wide", NEEDLE_LATE_W]]) {
        row("scan", "literal_end", w, 30000 * SCALE, n => {
            const re = /ZZTARGETZZ/; for (let i = 0; i < n; i++) re.test(s);
        });
        row("scan", "class_end", w, 300 * SCALE, n => {
            const re = /Z{2}TARGET/; for (let i = 0; i < n; i++) re.test(s);
        });
    }
}

/* ---- 3. character classes (REOP_range; bitmap + unroll proposals) ------ */
{
    const cases = [["small_range", /[a-z]+/g], ["word", /[a-zA-Z0-9_]+/g],
                   ["digit", /\d+/g], ["negated", /[^aeiou ]+/g],
                   ["wide_range", /[Ā-俿]/g]];
    for (const [nm, re] of cases)
        for (const [w, s] of [["narrow", SUBJ_N], ["wide", SUBJ_W]])
            row("class", nm, w, 900 * SCALE, n => {
                for (let i = 0; i < n; i++) { re.lastIndex = 0; let m, c = 0;
                    while ((m = re.exec(s)) && ++c < 200) {} }
            });
}

/* ---- 4. backreferences (the memcmp proposal) -------------------------- */
/* Long captured group so the compare length dominates dispatch, plus the
 * ignore-case variant which MUST keep the per-character path. */
{
    const long = "abcdefghijklmnopqrstuvwxyz0123456789".repeat(8);
    const subjN = long + long + " tail";
    const subjW = subjN + "中";
    for (const [w, s] of [["narrow", subjN], ["wide", subjW]]) {
        row("backref", "long", w, 30000 * SCALE, n => {
            const re = /^(.{288})\1/; for (let i = 0; i < n; i++) re.test(s);
        });
        row("backref", "long_i", w, 15000 * SCALE, n => {
            const re = /^(.{288})\1/i; for (let i = 0; i < n; i++) re.test(s);
        });
        row("backref", "short", w, 20 * SCALE, n => {   /* 1.7 ms/op: catastrophic, see note */
            const re = /(\w+) \1/; for (let i = 0; i < n; i++) re.test(s);
        });
    }
}

/* ---- 5. quantifiers, alternation, groups ------------------------------ */
{
    for (const [w, s] of [["narrow", SUBJ_N], ["wide", SUBJ_W]]) {
        row("quant", "greedy_dot", w, 1500 * SCALE, n => {
            const re = /^.*amet/; for (let i = 0; i < n; i++) re.test(s);
        });
        row("quant", "lazy_dot", w, 60000 * SCALE, n => {
            const re = /^.*?amet/; for (let i = 0; i < n; i++) re.test(s);
        });
        row("quant", "alternation", w, 700 * SCALE, n => {
            const re = /(lorem|ipsum|dolor|sit|amet|consectetur)/g;
            for (let i = 0; i < n; i++) { re.lastIndex = 0; let c = 0;
                while (re.exec(s) && ++c < 200) {} }
        });
        row("quant", "nested_group", w, 700 * SCALE, n => {
            const re = /((\w+)\s+(\w+))/g;
            for (let i = 0; i < n; i++) { re.lastIndex = 0; let c = 0;
                while (re.exec(s) && ++c < 200) {} }
        });
    }
}

/* ---- 6. captures (the memset + frame-packing proposals) --------------- */
/* Many capture groups so the per-match capture bookkeeping dominates. */
{
    const many = new RegExp("(" + "(\\w)".repeat(18) + ")");
    for (const [w, s] of [["narrow", SUBJ_N], ["wide", SUBJ_W]])
        row("capture", "many_groups", w, 150 * SCALE, n => {
            for (let i = 0; i < n; i++) many.exec(s);
        });
}

/* ---- 7. adversarial: catastrophic backtracking ------------------------ */
/* Kept permanently. A change that helps the happy path and worsens this is
 * not an improvement. Sized to stay well under a second. */
{
    row("adversarial", "nested_quant", "narrow", 10 * SCALE, n => {
        const re = /^(a+)+$/; const s = "a".repeat(20) + "b";
        for (let i = 0; i < n; i++) re.test(s);
    });
    row("adversarial", "alt_backtrack", "narrow", 160 * SCALE, n => {
        const re = /^(a|aa)+$/; const s = "a".repeat(18) + "b";
        for (let i = 0; i < n; i++) re.test(s);
    });
}

/* ---- 8. COMPILE time (the dbuf_insert / string-hash proposals) -------- */
/* Separate from exec on purpose. Big alternations and many quantifiers are the
 * shapes that hit the quadratic memmove in re_parse_term. The source string is
 * varied per iteration so the regexp cache cannot serve it. */
{
    function compileBench(name, build, reps) {
        row("compile", name, "n/a", reps, n => {
            for (let i = 0; i < n; i++) new RegExp(build(i));
        });
    }
    compileBench("small", i => "foo" + (i % 7) + "bar", 100000 * SCALE);
    compileBench("big_alternation", i => "(" +
        Array.from({length: 200}, (_, k) => "w" + k + "x" + (i % 3)).join("|") + ")",
        200 * SCALE);
    compileBench("many_quantifiers", i =>
        Array.from({length: 150}, (_, k) => "(a" + (k % 9) + (i % 2) + ")*").join(""),
        1200 * SCALE);
    compileBench("big_class", i => "[" +
        Array.from({length: 120}, (_, k) => String.fromCharCode(0x100 + k * 3)).join("") +
        String.fromCharCode(0x41 + (i % 20)) + "]+", 500 * SCALE);
    compileBench("nested_groups", i =>
        "(".repeat(40) + "a" + (i % 5) + ")".repeat(40), 20000 * SCALE);
    compileBench("unicode_prop", i => "\\p{L}+" + (i % 3), 100000 * SCALE);
}

print("#R DONE");
