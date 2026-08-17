/* Regex start-position scanning: the cost of REJECTING positions, not of matching.
 *
 * A non-sticky pattern walks every start position through the backtracking VM
 * (a split/any/goto loop compiled into the regex bytecode), so a search over a
 * long subject is dominated by rejected positions. `String.indexOf` on the same
 * needle is the floor: it is the same scan done by simd.strfind with nothing
 * else attached. The gap between the two is the whole prefilter opportunity.
 *
 * Cases are grouped by which prefilter kind they exercise so a regression can be
 * attributed:
 *   LIT  -- multi-char literal prefix   -> simd.strfind
 *   CHAR -- single literal first char   -> simd.find_u8 / find_u16
 *   SET  -- small leading char class    -> simd.find_first_of
 *   NONE -- must NOT be prefiltered (alternation/anchor/sticky); these are the
 *           guard rails: they must not get FASTER (that would mean the
 *           prefilter engaged where it must not) and must not get slower.
 *   FOLD -- ignore-case, which IS prefiltered and is expected to get faster.
 *           It used to be a NONE guard rail, and this comment still said so
 *           long after RE_PF_CASE_MIN made re_pf_build handle char_i/range_i:
 *           the file asserted the opposite of the code, so a real speedup here
 *           read as "the prefilter engaged where it must not".
 *
 * KNOWN GAP (measured neutral, not a regression): a leading class of more than 8
 * distinct bytes -- which includes `\d` == [0-9] -- gets no prefilter, because
 * simd.find_first_of only vectorises setlen<=8 and falls back to a scalar
 * 256-entry table above that. See "/[0-9]{4}-[0-9]{2}/" below sitting at parity.
 * Closing it needs a contiguous-RANGE kernel (`lo <= b <= hi` is two vector
 * compares, versus eight equality compares for a set of 8), which is a separate
 * change with its own measurement -- do not "fix" it by raising the set cap,
 * that just buys the scalar arm.
 */
function bench(name, f) {
    for (let i = 0; i < 3; i++) f();
    let best = Infinity;
    for (let r = 0; r < 7; r++) {
        const t0 = performance.now(); f(); const t1 = performance.now();
        if (t1 - t0 < best) best = t1 - t0;
    }
    console.log(name.padEnd(42) + best.toFixed(3) + " ms");
    return best;
}

const UNIT = "the quick brown fox jumps over the lazy dog ";
let hay = UNIT.repeat(4000);          /* ~172 KB, 8-bit */
const hayHit = hay + "NEEDLE_XYZ";
let wide = "naïve café — ünïcødé 日本語 ".repeat(4000);   /* 16-bit subject */
const wideHit = wide + "☃ZZ";

console.log("--- floor: the same scan with no regex engine attached ---");
const floorLit = bench("indexOf('NEEDLE_XYZ')", () => hayHit.indexOf("NEEDLE_XYZ"));
const floorChr = bench("indexOf('~')  (absent)", () => hay.indexOf("~"));

console.log("--- LIT: multi-char literal prefix ---");
bench("/NEEDLE_XYZ/ hit at end", () => /NEEDLE_XYZ/.exec(hayHit));
bench("/NEEDLE_XYZ/ no match", () => /NEEDLE_XYZ/.exec(hay));
bench("/needle\\d+/ no match", () => /needle\d+/.exec(hay));
bench("/quick brown/g count", () => hay.match(/quick brown/g).length);

console.log("--- CHAR: single literal first char ---");
bench("/~/ absent (8-bit)", () => /~/.exec(hay));
bench("/z(?=ebra)/ rare first char", () => /z(?=ebra)/.exec(hay));
bench("/\\u2603ZZ/ absent (16-bit)", () => /☃ZZ/.exec(wide));
bench("/\\u2603ZZ/ hit (16-bit)", () => /☃ZZ/.exec(wideHit));

console.log("--- SET: small leading char class ---");
bench("/[0-9]{4}-[0-9]{2}/ no match", () => /[0-9]{4}-[0-9]{2}/.exec(hay));
bench("/[~^]/ absent", () => /[~^]/.exec(hay));

console.log("--- NONE: must not be prefiltered (guard rails) ---");
bench("/(foo|NEEDLE_XYZ)/ alternation", () => /(foo|NEEDLE_XYZ)/.exec(hayHit));
bench("/^NEEDLE_XYZ/ anchored", () => /^NEEDLE_XYZ/.exec(hayHit));
bench("/\\bNEEDLE_XYZ\\b/ word bound", () => /\bNEEDLE_XYZ\b/.exec(hayHit));

/* FOLD: the fold enumeration runs 256 lre_canonicalize calls per re_pf_build,
 * 128 of them a binary search, and re_pf_build ran ONCE PER lre_exec. A single
 * .exec pays it once and barely shows it; the /g loops below pay it per MATCH,
 * which is where a per-exec rebuild actually costs something. char_i and
 * range_i are separate branches in re_pf_build and are timed separately. */
console.log("--- FOLD: ignore-case, IS prefiltered (was mislabelled NONE) ---");
bench("/NEEDLE_XYZ/i one exec", () => /NEEDLE_XYZ/i.exec(hayHit));
bench("/NEEDLE_XYZ/gi match-all", () => (hayHit.match(/NEEDLE_XYZ/gi) || []).length);
bench("/[q-t]uick/gi range_i /g", () => (hay.match(/[q-t]uick/gi) || []).length);
bench("/[wxyz]ebra/gi range_i absent", () => (hay.match(/[wxyz]ebra/gi) || []).length);
bench("/Q/gi char_i dense", () => (hay.match(/Q/gi) || []).length);
/* CONTROL for the two above: the same patterns WITHOUT /i must not move --
 * they take the case-sensitive branch, which this change does not touch. */
bench("/[q-t]uick/g  CONTROL", () => (hay.match(/[q-t]uick/g) || []).length);
bench("/Q/g  CONTROL", () => (hay.match(/Q/g) || []).length);

console.log("--- realistic: log/HTML scanning ---");
const log = ("127.0.0.1 - - [26/Jul/2026:10:00:00] \"GET /a/b HTTP/1.1\" 200 1234\n").repeat(3000);
bench("log: /\"GET [^\"]*\"/g", () => log.match(/"GET [^"]*"/g).length);
bench("log: /\\d+\\.\\d+\\.\\d+\\.\\d+/g", () => log.match(/\d+\.\d+\.\d+\.\d+/g).length);
const html = ("<div class='x'>text</div><p>more</p>").repeat(3000);
bench("html: /<\\/div>/g", () => html.match(/<\/div>/g).length);

console.log("\nfloor (indexOf literal): " + floorLit.toFixed(3) + " ms   " +
            "floor (indexOf char): " + floorChr.toFixed(3) + " ms");
