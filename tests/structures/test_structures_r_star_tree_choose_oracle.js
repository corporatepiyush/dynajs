// covers: dyna:structures.RStarTree
// ds_rs_choose (R* ChooseSubtree at the leaf-parent level) prunes its O(M^2) overlap-enlargement scan without changing
// the choice: an entry that already contains the new box has zero enlargement and zero overlap growth (no scan);
// an overlap term is computed only when the grown box meets the sibling (if it does not, neither did the old box); and
// the scan stops once the partial overlap growth exceeds the best so far (every term is >= 0, so the sum only grows).
// Correctness is checked against a brute-force scan (search = every stored box that intersects the query, nearest =
// the k smallest box distances). The pruning must not move any entry, so the order search() reports, which follows the
// tree's shape, is pinned to a fingerprint taken from the binary before the change (FNV-1a over the JSON of every
// search and nearest result below); a different ChooseSubtree decision changes that order.
// The tree's shape is platform-dependent: gcc fuses multiply-adds by default (gnu17), so area/overlap sums round
// differently and Linux builds a different (equally valid) tree. The pin is therefore keyed by platform: a57afa09 was
// taken on darwin/arm64 (clang) and 7f524277 on linux/arm64 (the docker gcc leg), each from the binary BEFORE the
// change and reproduced after it. A clang build on linux/arm64 contracts like darwin and builds the a57afa09 tree,
// so linux/arm64 accepts either pin. Elsewhere the pin prints a SKIP; the brute-force checks above run everywhere.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_r_star_tree_choose_oracle");
import { RStarTree } from "dyna:structures";
import { platform, arch } from "dyna:sys";

let checks = 0, fails = 0;
function eq(got, want, what) {
    checks++;
    if (got !== want) { fails++; print("FAIL: " + what + ": got " + got + ", want " + want); }
}
let seed = 99;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
let h = 2166136261;
const mix = (s) => { for (let i = 0; i < s.length; i++) h = Math.imul(h ^ s.charCodeAt(i), 16777619) >>> 0; };
const SHAPES = ["boxes", "points", "grid", "dup", "zero"];
function build(dims, shape, n) {
    const t = new RStarTree(dims), los = [], his = [];
    for (let i = 0; i < n; i++) {
        const lo = [], hi = [];
        for (let d = 0; d < dims; d++) {
            const a = shape === "grid" ? (i % 37) * 2 + d : shape === "dup" ? i % 5 : shape === "zero" ? (i % 2 ? -0 : 0) + ((i >> 1) % 3) : rnd() * 100;
            const w = shape === "points" || shape === "zero" ? 0 : shape === "grid" ? 1 : rnd() * 5;
            lo.push(a); hi.push(a + w);
        }
        t.insert(lo, hi, i);
        los.push(lo); his.push(hi);
    }
    return { t, los, his };
}
const boxDist = (lo, hi, p) => Math.sqrt(p.reduce((s, x, d) => { const g = x < lo[d] ? lo[d] - x : x > hi[d] ? x - hi[d] : 0; return s + g * g; }, 0));

__kitS.section("search and nearest equal a brute-force scan, and the search order matches the pre-change tree", () => {
    for (const dims of [1, 2, 3])
        for (const shape of SHAPES) {
            const { t, los, his } = build(dims, shape, 3000);
            for (let q = 0; q < 300; q++) {
                const lo = [], hi = [];
                for (let d = 0; d < dims; d++) { const a = rnd() * 100; lo.push(a); hi.push(a + rnd() * 20); }
                const got = t.search(lo, hi), near = t.nearest(lo, 3);
                mix(JSON.stringify(got));
                mix(JSON.stringify(near));
                const want = [];
                for (let i = 0; i < los.length; i++) if (los[i].every((x, d) => x <= hi[d] && his[i][d] >= lo[d])) want.push(i);
                eq(JSON.stringify([...got].sort((a, b) => a - b)), JSON.stringify(want), dims + "d " + shape + " search " + q);
                const dist = los.map((l, i) => boxDist(l, his[i], lo)).sort((a, b) => a - b);
                const nd = near.map((e) => e.distance), wd = dist.slice(0, 3);
                eq(nd.length === wd.length && nd.every((x, k) => Math.abs(x - wd[k]) <= 1e-12 * Math.max(1, wd[k])), true, dims + "d " + shape + " nearest " + q + " (distances within 1e-12; the C sum may fuse)");
            }
        }
    const PIN = { "darwin/arm64": ["a57afa09"], "linux/arm64": ["7f524277", "a57afa09"] }[platform() + "/" + arch()];
    if (PIN) { const got = h.toString(16); eq(got, PIN.includes(got) ? got : PIN.join(" or "), "search/nearest order fingerprint on " + platform() + "/" + arch() + " (pre-change binary)"); }
    else print("SKIP: no pre-change order fingerprint recorded for " + platform() + "/" + arch() + " (tree shape is platform-dependent); got " + h.toString(16));
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_structures_r_star_tree_choose_oracle: " + checks + " checks passed");
__kitS.end();
