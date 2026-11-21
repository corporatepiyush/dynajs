import { Hasher } from "dyna:hash";
import { Random } from "dyna:random";

const N = parseInt(scriptArgs[1] || "20000", 10);

for (let i = 0; i < N; i++) {
    const h = new Hasher(i % 2 ? "sha256" : "md5");
    h.update("some payload ").update(String(i));
    h.digestHex();

    const r = new Random(i);
    r.nextU64();
    r.nextBounded(100);
}

print("rss_plain_classes: churned " + N + " Hasher + Random instances");
