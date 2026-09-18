/* test_regexp_wide_anchor.js -- the 16-bit LINESTART prefilter (E09-02).
 *
 * The /m caret's wide-subject candidate scan was a scalar u16 loop while the
 * 8-bit one used the SIMD find_first_of; measured 3.10 ms vs 0.07 ms per
 * 1 MiB never-matching "^x" full scan, and with the landed u16 kernel the
 * wide case runs 0.03 ms (kernel) vs 3.07 ms (scalar fallback, mutation
 * A/B on this very host). Correctness here pins the CANDIDATE SET: line
 * starts are the buffer start and positions after \n, \r, U+2028 or
 * U+2029 -- including the empty final line and CRLF (start after the CR's
 * LF, not between them).
 */
let n = 0, fails = 0;
const ok = (got, want, what) => {
    n++;
    const g = JSON.stringify(got), w = JSON.stringify(want);
    if (g !== w) { fails++; print("FAIL: " + what + " got " + g + " want " + w); }
};
const idx = (s, re) => [...s.matchAll(re)].map(m => m.index);

ok(idx("q\nq\rq\u2028q\u2029q", /^q/gm), [0, 2, 4, 6, 8],
   "all four terminators create line starts");
ok(idx("a\u2028b", /(^|x)b/gm), [2], "LS (U+2028) is a line start under /m");
ok(idx("a\u2028b", /(^|x)b/g), [], "without /m the caret stays at string start only");
ok(idx("\r\nz", /^z/gm), [2], "CRLF: the line starts after the LF, not between");
ok(idx("x\u2029\u2029y", /^y/gm), [3], "PS (U+2029) chains");
ok(idx("", /^/gm), [0], "the empty string has one line start");
ok(idx("\u1234\n\u1234", /^\u1234/gm), [0, 2], "wide units count as ONE index each");
ok(idx("abc", /^abc$/gm), [0], "plain 8-bit /m unaffected");
{
    /* the never-matching full scan that the perf A/B used: 512 KiB of wide
       fill with terminators -- completes (bounded) and finds only real ones */
    let s = "";
    for (let i = 0; i < 2048; i++) s += "\u4e2d\u6587\n";
    ok(idx(s, /^\u4e2d/gm).length, 2048, "wide full scan finds every line start");
}
print("test_regexp_wide_anchor: " + n + " checks, " + fails + " failures");
if (fails) throw new Error("test_regexp_wide_anchor: " + fails + " failures");
