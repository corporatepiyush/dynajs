/* rss_plain_classes.js — peak-RSS-plateau leak proof for the plain GC classes.
 *
 * Hasher and Random have no close(): their lifetime is the GC's. LeakSanitizer is
 * unavailable on arm64-darwin, so the proof that dropping the last reference
 * really frees the native state is that PEAK RSS stays FLAT as the churn count
 * grows. Linear growth would mean the finalizer is not running (or not freeing).
 *
 *   ./dev.sh rss tests/rss_plain_classes.js          # 20k / 100k / 500k
 *
 * Requires CONFIG_NATIVE_MODULES=y. Reads the count from scriptArgs[1]. */
import { Hasher } from "dyna:hash";
import { Random } from "dyna:random";

const N = parseInt(scriptArgs[1] || "20000", 10);

for (let i = 0; i < N; i++) {
    /* a hasher: allocated, fed, digested, dropped unreferenced */
    const h = new Hasher(i % 2 ? "sha256" : "md5");
    h.update("some payload ").update(String(i));
    h.digestHex();

    /* a generator: dropped mid-stream, i.e. not "finished" in any sense */
    const r = new Random(i);
    r.nextU64();
    r.nextBounded(100);
}

print("rss_plain_classes: churned " + N + " Hasher + Random instances");
