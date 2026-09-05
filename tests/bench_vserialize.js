/* bench_vserialize.js -- the canonical-form key sort, and the clone memo.
 *
 * WHY THIS FILE EXISTS. venc_atom_less built and freed TWO heap strings per
 * comparison, inside an INSERTION sort -- so the sorted encoders were O(k^2)
 * comparisons each costing two mallocs and a UTF-8 conversion. Nothing in the
 * tree measured it: there was no vserialize benchmark at all, so the cost was
 * invisible and so is any regression in the replacement.
 *
 * WHAT MOVES AND WHAT MUST NOT. Only the SORTED encoders touch the key sort:
 *   CBORCanonical, ValueHash          -> the sorted path, the win
 *   CBOREncode, MsgPackEncode         -> venc_map_plain, the CONTROL
 * The control is the old emit loop verbatim. If it moves, the extraction
 * changed the unsorted path and that is a regression, not a win.
 *
 * KEY WIDTH IS THE OTHER AXIS. An ASCII key hits JS_AtomBorrowASCII and costs
 * no allocation even in the old code; a non-ASCII key goes through
 * JS_AtomToCStringLen and allocates. The utf8 rows are where the old code paid
 * twice per comparison, so they should move most -- if ascii and utf8 move by
 * the same amount, the win is not the one being claimed.
 *
 * SIZES straddle VENC_FAST_KEYS = 128, the stack/heap threshold for the
 * property table: 16 and 64 use the stack buffer, 256 and 1024 malloc it.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/bench_vserialize.js
 */
import { MsgPackEncode, CBOREncode, CBORCanonical, ValueHash,
         structuredClone } from "dyna:serialize";

const MIN_MS = 120;
let sink = 0;

/* Scale reps until the timed region clears the clock's noise floor, then take
 * the best of five. Returns ms for ONE call of fn(). */
function ms(fn) {
    fn(); fn();
    let mult = 1, dt;
    for (;;) {
        const t0 = performance.now();
        for (let m = 0; m < mult; m++) sink += fn();
        dt = performance.now() - t0;
        if (dt >= MIN_MS || mult >= 1 << 16) break;
        mult = Math.max(mult * 2, Math.ceil(mult * MIN_MS / Math.max(dt, 0.001)));
    }
    let best = Infinity;
    for (let k = 0; k < 5; k++) {
        const t0 = performance.now();
        for (let m = 0; m < mult; m++) sink += fn();
        const d = performance.now() - t0;
        if (d < best) best = d;
    }
    return best / mult;
}

/* Results go to a sink so nothing here can be eliminated: reading .length of a
 * Uint8Array forces the encode, reading .length of the hash forces the digest. */
const use = (v) => (typeof v === "string" ? v.length : v.length);

function mkObj(n, wide) {
    const o = {};
    for (let i = 0; i < n; i++) o[(wide ? "鍵値" : "key") + i + "_x"] = i;
    return o;
}

/* Insertion order is SHUFFLED deterministically: a sort measured on
 * already-sorted input measures its best case, and insertion sort's best case
 * is linear -- which would hide exactly the cost this file exists to show. */
function mkShuffled(n, wide) {
    const idx = [];
    for (let i = 0; i < n; i++) idx.push(i);
    for (let i = n - 1; i > 0; i--) {          /* fixed LCG, no Math.random */
        const j = (i * 1103515245 + 12345) % (i + 1);
        const t = idx[i]; idx[i] = idx[j]; idx[j] = t;
    }
    const o = {};
    for (const i of idx) o[(wide ? "鍵値" : "key") + i + "_x"] = i;
    return o;
}

print("=== vserialize: canonical key sort ===");
print("#V op            keys  width  ms/call");

const SIZES = [16, 64, 256, 1024];
for (const wide of [false, true]) {
    const w = wide ? "utf8 " : "ascii";
    for (const n of SIZES) {
        const o = mkShuffled(n, wide);
        const rows = [
            ["CBORCanonical", () => use(CBORCanonical(o))],
            ["ValueHash    ", () => use(ValueHash(o))],
            ["CBOREncode*  ", () => use(CBOREncode(o))],      /* CONTROL */
            ["MsgPackEnc*  ", () => use(MsgPackEncode(o))],   /* CONTROL */
        ];
        for (const [name, fn] of rows)
            print("#V " + name + " " + String(n).padStart(5) + "  " + w +
                  "  " + ms(fn).toFixed(4));
    }
}
print("(* = CONTROL: the unsorted path, must not move)");

/* ---- worst case for a comparison sort: keys that share a long prefix ----
 * Every comparison then runs the full memcmp instead of deciding on byte 1,
 * and in the old code every comparison also allocated both strings first. */
print("");
print("#V op            keys  shape        ms/call");
{
    const P = "common_prefix_that_forces_a_full_compare_";
    for (const n of [64, 256]) {
        const o = {};
        for (let i = 0; i < n; i++) o[P + String(i).padStart(6, "0")] = i;
        print("#V CBORCanonical " + String(n).padStart(5) + "  shared-prefix " +
              ms(() => use(CBORCanonical(o))).toFixed(4));
    }
}

/* ---- structuredClone memo: O(N^2) linear scan over visited objects ----
 * NOT YET FIXED -- this row is here to size the problem before anyone changes
 * it, and to be the baseline the fix is diffed against. Distinct child objects
 * are what grow the memo; repeating one child would measure a memo HIT. */
print("");
print("#V op            nodes  ms/call");
for (const n of [16, 128, 512, 2048]) {
    const root = {};
    for (let i = 0; i < n; i++) root["c" + i] = { v: i };
    print("#V clone         " + String(n).padStart(5) + "  " +
          ms(() => structuredClone(root) && 1).toFixed(4));
}

/* A cycle: the memo is load-bearing for correctness here, not just speed. */
{
    const a = { name: "a" }; a.self = a;
    const c = structuredClone(a);
    if (c.self !== c) throw new Error("bench_vserialize: clone memo lost a cycle");
}

if (sink === -1) print("unreachable");
print("done");
