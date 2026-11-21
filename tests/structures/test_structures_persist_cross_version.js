// covers: contract dyna:structures.RBTree dyna:structures.WBTree dyna:structures.SkipList dyna:structures.Table -- files saved before the streaming save writer still load, and new saves keep the v1 bytes
// tests/lib/persist_v1_fixtures.js holds <Class>-300 files written by the build before the streaming tree writer
// and the Table prefix sort (fixA1). A new save of the same instance must equal the fixture byte
// for byte except the per-save lineage nonce (header slot A +48..+55, file offsets 112..119: an
// xxh64 of time, pid and a stack address) and that slot's crc32c (slot A +128..+131, file offsets
// 192..195). Identical bytes everywhere else is what lets the older build load a new file.
import { suite } from "../lib/kit.js";
import { PERSIST } from "../lib/persist_build.js";
import * as S0 from "dyna:structures";
import { Graph as Graph0 } from "dyna:graph";
const S = { ...S0, Graph: Graph0 };
import * as file from "dyna:file";
import { PERSIST_V1 } from "../lib/persist_v1_fixtures.js";

const t = suite("persist_cross_version");
const N = 300;
const VOLATILE = new Set([112, 113, 114, 115, 116, 117, 118, 119, 192, 193, 194, 195]);
const unhex = (h) => new Uint8Array(h.match(/../g).map((x) => parseInt(x, 16)));

for (const cls of ["RBTree", "WBTree", "SkipList", "Table"]) {
    t.test(cls + " v1 fixture loads and new saves keep its bytes", (a) => {
        const tmp = file.makeTempDir("xver");
        const fx = tmp.join("old.dp");
        file.writeFile(fx, unhex(PERSIST_V1[cls]));
        const want = PERSIST[cls].build(N);
        const got = S[cls].load(fx);
        a.deep(JSON.stringify(PERSIST[cls].probe(got, N)), JSON.stringify(PERSIST[cls].probe(want, N)), "probe after loading the old file");
        a.deep([...got.serialize()].join(","), [...want.serialize()].join(","), "serialize after loading the old file");
        try {
            const p = tmp.join("n.dp");
            want.save(p, { fsync: false });
            const old = file.readBytes(fx), now = file.readBytes(p);
            a.eq(now.length, old.length, "file length");
            const diff = [];
            for (let i = 0; i < Math.min(old.length, now.length); i++) if (old[i] !== now[i] && !VOLATILE.has(i)) diff.push(i);
            a.deep(diff, [], "offsets that differ outside the lineage nonce and header crc");
        } finally {
            file.removeAll(tmp);
        }
    });
}

await t.run();
