// Random.shuffle has three implementations: a typed array is shuffled in its
// buffer, a plain (fast) array in its element storage, and any other array
// through property descriptors. All three run the same Fisher-Yates with the
// same draws, so one seed must give one permutation whichever path a value
// takes. The typed-array path is the reference; it predates the fast path.
import { Random } from "dyna:random";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };

for (const n of [2, 3, 7, 64, 1000, 50000]) {
    const plain = Array.from({ length: n }, (_, i) => i);
    const typed = Uint32Array.from(plain);
    const slow = Array.from({ length: n }, (_, i) => i);
    Object.defineProperty(slow, "0", { value: 0, writable: true, enumerable: true, configurable: true });
    slow.extra = 1;
    delete slow.extra;
    const sparseFirst = [];
    sparseFirst[n - 1] = n - 1;
    for (let i = 0; i < n - 1; i++) sparseFirst[i] = i;
    new Random(77).shuffle(plain);
    new Random(77).shuffle(typed);
    new Random(77).shuffle(slow);
    new Random(77).shuffle(sparseFirst);
    const t = Array.from(typed);
    ok(plain.every((v, i) => v === t[i]), "n=" + n + ": the plain-array permutation equals the typed-array one");
    ok(slow.every((v, i) => v === t[i]), "n=" + n + ": an array built by defineProperty gives the same permutation");
    ok(sparseFirst.every((v, i) => v === t[i]), "n=" + n + ": an array filled back to front gives the same permutation");
    ok(plain.slice().sort((a, b) => a - b).every((v, i) => v === i), "n=" + n + ": the shuffle is a permutation");
}
const objs = Array.from({ length: 100 }, (_, i) => ({ i }));
new Random(5).shuffle(objs);
ok(new Set(objs).size === 100 && objs.every((o) => typeof o.i === "number"), "object elements survive the swap with their identity");

print("test_random_shuffle_paths: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_random_shuffle_paths failed");
