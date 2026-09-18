/* bench_path_ab.js -- what did Path-only ACTUALLY cost or save?
 *
 * This exists because a claim was made here without measurement and turned out
 * to be false. The claim was that `Path`'s real win is at the C boundary --
 * that the string API had to scan, malloc and copy every path into UTF-8 on
 * every filesystem call, and that borrowing a Path's already-normalised bytes
 * removed that. It is a plausible story and it is wrong by an order of
 * magnitude, because the syscall dominates: coercing a 60-byte path is tens of
 * nanoseconds against a microsecond of lstat.
 *
 * It could not be measured from inside the new tree, because the string API had
 * already been deleted -- which is exactly the situation CLAUDE.md sec.14 calls
 * out: a comparison against a function that no longer exists is not stale, it
 * is wrong. The measurement has to be an A/B against a REAL pre-Path binary.
 *
 *   git checkout 4ca8e2f          # the commit before Path landed
 *   make clean && make CONFIG_NATIVE_MODULES=y && cp dynajs /tmp/dynajs-old
 *   git checkout stdlib-core-extraction
 *   make clean && make CONFIG_NATIVE_MODULES=y
 *   ./dynajs tests/bench_path_ab.js            # new side
 *   /tmp/dynajs-old tests/bench_path_ab.js     # old side (edit USE_PATH below)
 *
 * MEASURED -- five INTERLEAVED pairs, means in microseconds:
 *
 *     op         string path (old)   borrowed Path (new)   ratio
 *     exists           1.573               1.568           0.997x
 *     stat             2.277               2.275           0.999x
 *     readFile        12.570              12.449           0.990x
 *
 * So Path-only COSTS NOTHING, which is the bar the programme actually sets
 * ("negligible, not faster"), and it is NOT a speed-up. Both halves matter.
 *
 * INTERLEAVE THE RUNS. Measuring all of one side and then all of the other
 * first reported readFile 3-6% slower on the new binary, which is a number
 * that would have been believed and acted on. Interleaving showed it as drift:
 * this bench takes ~20 s a side and the machine does not hold still that long.
 *
 * AND CHECK WHICH BINARY YOU ARE RUNNING. The first attempt reported OLD on
 * both sides -- the branch had been checked out but not rebuilt, so ./dynajs
 * was still the pre-Path build (CLAUDE.md sec.14: a shared build artefact is
 * shared mutable state). The #AB tag exists for exactly that reason, and it is
 * what caught it.
 */
import * as file from "dyna:file";

/* The old binary has no Path export; the new one does. Detecting it keeps ONE
 * file runnable on both sides, so the two columns cannot drift apart. */
const USE_PATH = typeof file.Path === "function";
const mk = (root, name) => USE_PATH ? root.join(name) : root + "/" + name;

const root = file.makeTempDir("dyna-path-ab-");
const p = mk(root, "f.txt");
file.writeFile(p, "x".repeat(256));

function T(fn, n) {
    for (let i = 0; i < Math.min(n, 20000); i++) fn();     /* warm */
    const t = performance.now();
    for (let i = 0; i < n; i++) fn();
    return (performance.now() - t) * 1000 / n;
}

const tag = USE_PATH ? "NEW(Path)" : "OLD(string)";
print(`#AB\t${tag}\texists\t${T(() => file.exists(p), 200000).toFixed(3)}`);
print(`#AB\t${tag}\tstat\t${T(() => file.stat(p), 100000).toFixed(3)}`);
print(`#AB\t${tag}\treadFile\t${T(() => file.readFile(p), 100000).toFixed(3)}`);

file.removeAll(root);
