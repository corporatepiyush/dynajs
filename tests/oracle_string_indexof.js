/* oracle_string_indexof.js -- the differential for routing string_indexof
 * through the SIMD substring kernel.
 *
 * The change touches `indexOf`, `includes`, `split`, `replace`, `replaceAll`
 * and the four new `String.prototype` search methods at once, so "the tests
 * pass" is not evidence: it has to produce the SAME ANSWER as the scalar path
 * on every input, not merely a plausible one.
 *
 * Per CLAUDE.md section 2, the way to know that is to compile the same source
 * twice -- once with the optimisation `#ifndef`-gated off -- and diff the
 * outputs. This file emits one line per case; run it under both binaries and
 * `diff` the results:
 *
 *     make clean && make CONFIG_NATIVE_MODULES=y
 *     ./dynajs tests/oracle_string_indexof.js > /tmp/new.txt
 *     make clean && make CONFIG_NATIVE_MODULES=y CFLAGS_EXTRA=-DDYN_NO_SIMD_STRFIND
 *     ./dynajs tests/oracle_string_indexof.js > /tmp/old.txt
 *     diff /tmp/old.txt /tmp/new.txt
 *
 * The corpus deliberately includes the cases where a substring kernel and a
 * first-character scan diverge if either is wrong: an empty needle (which must
 * match at `from`, not at 0), a needle longer than the haystack, a `from` past
 * the end, needles that repeat their own first character, and WIDE strings,
 * which must fall through to the scalar path because the kernel is byte-wise.
 */

let lines = 0;
function emit(s) { lines++; print(s); }

const HAY = [
    "", "a", "aa", "aaa", "abcabcabc", "mississippi",
    "the quick brown fox jumps over the lazy dog",
    "aaaaaaaaab", "ababababab", "x".repeat(100) + "needle" + "y".repeat(100),
    "éèê",                       /* Latin-1: still narrow */
    "café au lait",
    "日本語",                       /* wide: the scalar path */
    "a日b本c",
    "\u{1F600}\u{1F600}x",                      /* surrogate pairs */
    "mixed café 日本 \u{1F600} end",
    "\0embedded\0nul\0",
];
const NEEDLE = [
    "", "a", "aa", "aaa", "ab", "abc", "b", "z", "ss", "issi",
    "needle", "the", "fox", "dog", "é", "日", "\u{1F600}",
    "\0", "nul", "x".repeat(50), "aaaaaaaaab", "lait",
];
const FROM = [undefined, -5, 0, 1, 2, 5, 50, 1000];

for (const h of HAY) {
    for (const n of NEEDLE) {
        for (const f of FROM) {
            const a = f === undefined ? h.indexOf(n) : h.indexOf(n, f);
            const b = f === undefined ? h.lastIndexOf(n) : h.lastIndexOf(n, f);
            const c = f === undefined ? h.includes(n) : h.includes(n, f);
            emit(`i ${JSON.stringify(h)} ${JSON.stringify(n)} ${f} ${a} ${b} ${c}`);
        }
        /* the consumers that go through the same search */
        emit(`s ${JSON.stringify(h.split(n))}`);
        emit(`r ${JSON.stringify(h.replace(n, "<>"))} ${JSON.stringify(h.replaceAll(n, "<>"))}`);
        emit(`x ${JSON.stringify(h.indexOfAll(n))} ${JSON.stringify(h.splitN(n, 2))}`);
        emit(`t ${h.startsWith(n)} ${h.endsWith(n)} ${JSON.stringify(h.trimPrefix(n))} ${JSON.stringify(h.trimSuffix(n))}`);
    }
}

/* A randomised sweep over a small alphabet, which is where a substring search
 * is most likely to disagree with itself: many partial matches, many restarts. */
function rng(seed) {
    let s = seed >>> 0;
    return () => { s = (Math.imul(s, 1103515245) + 12345) >>> 0; return s / 4294967296; };
}
{
    const r = rng(20260727);
    const alpha = "aab";
    for (let k = 0; k < 20000; k++) {
        let h = "", n = "";
        const hl = 1 + Math.floor(r() * 40), nl = 1 + Math.floor(r() * 6);
        for (let i = 0; i < hl; i++) h += alpha[Math.floor(r() * alpha.length)];
        for (let i = 0; i < nl; i++) n += alpha[Math.floor(r() * alpha.length)];
        const from = Math.floor(r() * (hl + 2));
        emit(`R ${h.indexOf(n, from)} ${h.lastIndexOf(n)} ${JSON.stringify(h.indexOfAll(n))}`);
    }
}

/* The same sweep with a wide character mixed in, so the scalar fall-through is
 * covered by more than the handful of literals above. */
{
    const r = rng(99);
    const alpha = "ab日";
    for (let k = 0; k < 20000; k++) {
        let h = "", n = "";
        const hl = 1 + Math.floor(r() * 30), nl = 1 + Math.floor(r() * 4);
        for (let i = 0; i < hl; i++) h += alpha[Math.floor(r() * alpha.length)];
        for (let i = 0; i < nl; i++) n += alpha[Math.floor(r() * alpha.length)];
        emit(`W ${h.indexOf(n)} ${h.lastIndexOf(n)} ${JSON.stringify(h.split(n))}`);
    }
}

print("# oracle_string_indexof: " + lines + " cases");
