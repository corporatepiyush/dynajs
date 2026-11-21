// covers: contract dyna:structures.* -- every class at every size refuses damaged persist files with a stable error code
// For every class at N = 128, 1K, 10K, 50K and 100K a saved file is damaged on
// disk and loaded in copy and mmap mode. The section table is read from the
// active header slot so each damage lands on a real section:
//   one flipped byte inside every section      -> EPERSIST_CHECKSUM
//   truncation at every section boundary/middle -> EPERSIST_TRUNCATED
//   a flipped superblock byte                   -> EPERSIST_CHECKSUM
//   both header slots damaged                   -> EPERSIST_CHECKSUM
//   a damaged inactive slot B                   -> loads (slot A is current)
//   superblock version 2                        -> EPERSIST_VERSION
//   loading as another class                    -> EPERSIST_TYPE naming both
//   trailing garbage after the logical end      -> loads (crash leftovers)
// With verify: false a flipped section byte must either load or throw an Error
// with a code; it must never crash, which the san and leaks legs check. For the
// classes stored as one codec record, the record's payload is also rewritten
// and its DYNS trailer crc re-sealed, so the damage reaches the decoder itself:
// whatever the decoder throws must come out with code EPERSIST_CORRUPT.
import * as S0 from "dyna:structures";
import { Graph as Graph0 } from "dyna:graph";
const S = { ...S0, Graph: Graph0 };
import { makeTempDir, removeAll, readBytes, writeFile } from "dyna:file";
import { CRC32C } from "dyna:hash";
import { suite } from "../lib/kit.js";
import { PERSIST, SIZES } from "../lib/persist_build.js";

const t = suite("structures_persist_corrupt");
const dir = makeTempDir("persist-bad-");
t.after(() => removeAll(dir));

const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
const u64 = (b, o) => u32(b, o) + u32(b, o + 4) * 4294967296;

function layout(b) {
    const slot = u32(b, 16), h = 64, ns = u32(b, h + 24), secs = [];
    for (let i = 0; i < ns; i++) {
        const e = h + 160 + 40 * i;
        secs.push({ off: u64(b, e + 16), len: u64(b, e + 24) });
    }
    return { slot, fileSize: u64(b, h + 40), secs };
}

function codeOf(fn) {
    try { fn(); return "loaded"; } catch (e) { return e && e.code ? e.code : "uncoded:" + String(e); }
}

const names = Object.keys(PERSIST);

for (const name of names) {
    const C = S[name], d = PERSIST[name];
    const other = names[(names.indexOf(name) + 1) % names.length];
    const lopt = (mmap, extra) => Object.assign(d.arg ? { mmap, compare: d.arg } : { mmap }, extra || {});
    t.test(name + " damaged files at every size", (a) => {
        for (const n of SIZES) {
            const good = dir.join(name + "-" + n + ".dp"), bad = dir.join(name + "-" + n + "-bad.dp");
            d.build(n).save(good, { fsync: false });
            const img = readBytes(good), L = layout(img);
            a.must(L.secs.length > 0 && L.fileSize === img.length, name + " n=" + n + " layout parses");
            const tryAll = (bytes, want, what, extra) => {
                writeFile(bad, bytes);
                for (const mmap of [false, true]) {
                    const got = codeOf(() => C.load(bad, lopt(mmap, extra)));
                    a.eq(got, want, name + " n=" + n + (mmap ? " mmap " : " copy ") + what);
                }
            };
            L.secs.forEach((s, i) => {
                if (!s.len) return;
                const f = img.slice();
                f[s.off + Math.floor(s.len / 2)] ^= 0x40;
                tryAll(f, "EPERSIST_CHECKSUM", "flip in section " + i);
                writeFile(bad, f);
                const loose = codeOf(() => C.load(bad, lopt(false, { verify: false })));
                a.ok(loose === "loaded" || loose.startsWith("E"), name + " n=" + n + " verify:false flip gives a coded result, got " + loose);
            });
            const cuts = new Set([L.secs[0].off - 1]);
            for (const s of L.secs) {
                cuts.add(s.off);
                cuts.add(s.off + Math.floor(s.len / 2));
                if (s.off + s.len < img.length) cuts.add(s.off + s.len);
            }
            cuts.add(img.length - 1);
            for (const c of cuts) if (c > 64 + 2 * L.slot && c < img.length) tryAll(img.slice(0, c), "EPERSIST_TRUNCATED", "cut at " + c);
            let f = img.slice(); f[20] ^= 1;
            tryAll(f, "EPERSIST_CHECKSUM", "superblock flip");
            f = img.slice(); f[64 + 70] ^= 1; f[64 + L.slot + 70] ^= 1;
            tryAll(f, "EPERSIST_CHECKSUM", "both slots damaged");
            f = img.slice(); f[64 + L.slot + 70] ^= 1;
            tryAll(f, "loaded", "inactive slot B damaged");
            f = img.slice(); f[8] = 2;
            tryAll(f, "EPERSIST_VERSION", "version 2");
            f = new Uint8Array(img.length + 777); f.set(img); f.fill(0xee, img.length);
            tryAll(f, "loaded", "trailing garbage");
            if (L.secs.length === 1 && L.secs[0].len > 24) {
                const s0 = L.secs[0], seen = new Set();
                for (const at of [20, 21, 24, 28, Math.floor(s0.len / 2)]) {
                    if (at >= s0.len - 4) continue;
                    f = img.slice();
                    f[s0.off + at] ^= 0xff;
                    const rec = f.subarray(s0.off, s0.off + s0.len);
                    new DataView(rec.buffer, rec.byteOffset, rec.length).setUint32(rec.length - 4, CRC32C(rec.subarray(0, rec.length - 4)) >>> 0, true);
                    writeFile(bad, f);
                    const got = codeOf(() => C.load(bad, lopt(false, { verify: false })));
                    seen.add(got);
                    a.ok(got === "loaded" || got === "EPERSIST_CORRUPT", name + " n=" + n + " re-sealed record damage at " + at + " -> " + got);
                }
                if (n === SIZES[SIZES.length - 1]) a.note(name + " re-sealed outcomes " + [...seen].join(","));
            }
            let e = null;
            try { S[other].load(good, PERSIST[other].arg ? { compare: PERSIST[other].arg } : {}); } catch (x) { e = x; }
            a.ok(e && e.code === "EPERSIST_TYPE" && e.message.includes(name) && e.message.includes(other),
                name + " n=" + n + " load as " + other + " -> " + (e && e.code) + " " + (e && e.message));
        }
    });
}

await t.run();
