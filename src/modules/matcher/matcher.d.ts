/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:matcher
/** dyna:matcher — text search and similarity: compiled single- and multi-pattern (Aho-Corasick) matchers, edit distances, tries, Bloom filters, diffs. */
declare module "dyna:matcher" {
    /** A compiled single-pattern matcher. */
    class Matcher {
        /** `algo` is "kmp" (default) or "bmh" (alias "boyer-moore"); a pure-ASCII pattern searches the subject's bytes directly. */
        constructor(pattern: string, opts?: { algo?: "kmp" | "bmh" | "boyer-moore" });
        /** Code-unit offset of the first match from `fromIndex` (String.prototype.indexOf conventions), or -1. */
        firstIn(text: string, fromIndex?: number): number;
        /** True when a pattern occurs in `text`. */
        test(text: string): boolean;
        /** Number of matches in `text`. */
        countIn(text: string): number;
        /** Every match offset. */
        allIn(text: string): number[];
        /** Non-overlapping left-to-right replacement. */
        replaceAllIn(text: string, repl: string): string;
        readonly length: number;
        readonly algo: string;
    }

    /** A byte-trie Aho-Corasick multi-pattern matcher. Identical patterns are deduplicated: N identical hits at one position emit one, at the first index. */
    class MultiMatcher {
        constructor(patterns: string[]);
        /** The pattern index and offset of the earliest hit, or null. */
        firstIn(text: string): { index: number; at: number } | null;
        /** True when a pattern occurs in `text`. */
        test(text: string): boolean;
        /** Every emitted hit (overlapping matches each count). */
        countIn(text: string): number;
        /** Every hit as { index: pattern number, at: position }, ordered by end position (longest first at one end). */
        allIn(text: string): { index: number; at: number }[];
        /** Replaces non-overlapping hits left to right in one pass; `repl` is a string or repl(match, patternIndex). */
        replaceAllIn(text: string, repl: string | ((match: string, index: number) => string)): string;
        /** Writes every hit of the patterns in `buf` (UTF-8 bytes) into caller-owned Int32Arrays and returns the TOTAL hit count; no per-hit JS allocation, synchronous.
         *  Offsets are byte offsets into `buf`, end exclusive (outStarts[i] <= outEnds[i]); outIds[i] is the pattern index. Identical patterns report only the first index.
         *  Default: every overlapping hit in allIn order (by end, longest first at one end). `longestOnly`: leftmost-longest non-overlapping, ordered by start: at the
         *  leftmost start take the longest pattern, then resume at its end; a tie at one start and length is one byte string, so the lowest pattern index wins.
         *  Capacity: when the total exceeds the arrays' common length, the first `length` hits are written, the rest of the scan only counts, and the total is still
         *  returned, so the caller can grow the arrays to the returned count and call again; slots past the written prefix are left untouched.
         *  TypeError: buf not a Uint8Array, an out array not an Int32Array, out lengths differ, a detached or out-of-bounds view (a resizable buffer is read at its
         *  current length), an immutable out buffer, any two of the four views sharing memory, opts not a plain object, an unknown opts key, a non-boolean longestOnly.
         *  RangeError: buf longer than 2^31 - 1 bytes; past the scan step budget the same RangeError as allIn. Options are read before the views. */
        matchSpansInto(buf: Uint8Array, outStarts: Int32Array, outEnds: Int32Array, outIds: Int32Array, opts?: { longestOnly?: boolean }): number;
        readonly size: number;
        readonly states: number;
    }

    /** Exact edit distance in code points, Myers bit-parallel below 64. */
    function Levenshtein(a: string, b: string, opts?: { max?: number }): number;
    /** Bigram multiset similarity in [0, 1]. */
    function DiceCoefficient(a: string, b: string): number;
    /** Jaro similarity with Winkler's prefix boost (prefix capped at 4, scale 0.1) in [0, 1]; code-point operands. */
    function JaroWinkler(a: string, b: string): number;
    /** Restricted Damerau-Levenshtein (optimal string alignment) in code points; exact while <= max, max + 1 beyond. */
    function DamerauLevenshtein(a: string, b: string, opts?: { max?: number }): number;
    /** Local alignment (linear gap) of two strings by code point, or two 4-byte typed arrays by raw 32-bit token; ties prefer diagonal, then a-gap (I), then b-gap (D).
     *  band/diagonal keep only cells with j - i in [diagonal - band, diagonal + band]; ranges are half-open; cigar uses M/I/D (I consumes a, D consumes b). */
    function SmithWaterman(a: string | Int32Array | Uint32Array, b: string | Int32Array | Uint32Array, opts?: { match?: number; mismatch?: number; gap?: number; band?: number; diagonal?: number; traceback?: boolean }):
        { score: number; aStart: number; aEnd: number; bStart: number; bEnd: number; matches?: number; cigar?: string };
    /** A diff hunk: -1 deleted, 1 inserted, 0 common. */
    interface DiffHunk {
        op: -1 | 0 | 1;
        text: string;
    }
    /** Options of DiffChars, DiffWords and DiffLines: only the dyna meta bag. `dyna.maxSteps` raises the step budget's ceiling (an integer in [2^31, 2^53-1]; a smaller value is a RangeError because the meta config can only raise it) or removes the budget with Infinity: plain Myers then runs to the end with no fallback, O((N+M)D) time. Unknown keys are a TypeError. */
    interface DiffOptions {
        dyna?: { maxSteps?: number };
    }
    /** Myers diff by character in linear space (middle snake, explicit work stack): memory is O(N+M) tokens. The step budget is 2(N+M)^2 diagonal steps clamped to [2^28, 2^31] (opts.dyna.maxSteps raises the ceiling or removes it). Past the budget, a pair whose stripped sides multiply to at most 2^24 cells gets the longest-common-subsequence script instead (computed with row checkpoints, still O(N+M) memory; the script prefers a match, then an insertion); a larger pair throws RangeError("inputs too dissimilar"). */
    function DiffChars(a: string, b: string, opts?: DiffOptions): DiffHunk[];
    /** Myers diff tokenised by word; same linear-space Myers, budget, fallback and opts.dyna.maxSteps. */
    function DiffWords(a: string, b: string, opts?: DiffOptions): DiffHunk[];
    /** Myers diff tokenised by line; same linear-space Myers, budget, fallback and opts.dyna.maxSteps. */
    function DiffLines(a: string, b: string, opts?: DiffOptions): DiffHunk[];
    /** Unified diff text of two texts by line (DiffLines' Myers script and budget), in `diff -u` format: "--- aName\n+++ bName\n" (defaults "a", "b"; no timestamps; a name may not hold a line break or NUL), then hunks "@@ -l,n +l,n @@" (",1" omitted; an empty range names the line before it, so an empty side is -0,0) with `context` unchanged lines around each change (default 3, integer 0..2^30; changes at most 2*context apart share a hunk); deletions precede insertions within a change. A last line without "\n" is followed by "\ No newline at end of file". Identical texts give "". `dyna.maxSteps` is DiffLines' meta option. Options are strict (TypeError naming the valid set). */
    function UnifiedDiff(a: string, b: string, opts?: { context?: number; aName?: string; bName?: string; dyna?: { maxSteps?: number } }): string;
    /** Applies one file's unified diff (UnifiedDiff's, `diff -u`'s or git's) to `text`; ApplyPatch(a, UnifiedDiff(a, b)) === b. Lines before the first "@@" and after a hunk's counted lines (diff/index/---/+++ headers, mail signatures) are skipped; an empty patch line inside a hunk is an empty context line; "\ No newline at end of file" makes the line above it end without "\n". Hunks apply strictly at the old-side line numbers they state (no offset search, no fuzz), in order: a context or removed line that differs from the text (its final newline included) is an Error naming the hunk and line, as is a hunk before the previous one's end or past the text. A malformed header, a truncated hunk, an unexpected line tag or a second file ("--- " + "+++ " after a hunk) is a SyntaxError naming the patch line. An empty patch returns `text`. */
    function ApplyPatch(text: string, patch: string): string;
    /** The Snowball English (Porter2) stemmer, https://snowballstem.org/algorithms/english/stemmer.html, as Snowball 3 generates it: every one of
     *  the 42,649 pairs of the snowball-data english voc.txt -> output.txt reference matches (tests/corpus/snowball/english). It covers the 3.x
     *  rules: R1 prefixes gener commun arsen past univers later emerg organ inter, -ogist -> og, Cying -> Cie, no undoubling after a single letter
     *  (add, ebb, err, off), "evening" invariant. The word is taken as given: pass it lowercase; letters other than a-z and the apostrophe are
     *  non-vowels, and lengths and regions are counted in code points ("éying" -> "éie"). A non-string is a TypeError. */
    function stem(word: string): string;
}