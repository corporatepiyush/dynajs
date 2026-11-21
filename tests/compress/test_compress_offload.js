// covers: integration dyna:async.offload dyna:compress.gzip dyna:compress.gunzip dyna:compress.zstd dyna:compress.unzstd dyna:compress.brotli dyna:compress.unbrotli dyna:compress.snappy dyna:compress.unsnappy dyna:compress.lz4Compress dyna:compress.lz4Decompress dyna:compress.lz4Frame dyna:compress.lz4Unframe dyna:compress.deflate dyna:compress.inflate -- the offload contract (byte-identical result, loop ticks) checked for every marked function of the module
// Owner ask E, wave 4: every one-shot codec of dyna:compress marks its codec call DYN_OFFLOAD_CPU, so under
// offload() it runs as a CPU job on the shared io pool (--io-threads) while the loop keeps serving. Per codec:
// the offloaded result is byte-identical to the direct call (oracle: the direct call, plus the round trip back to
// the input), and the promise is still pending when offload() returns (a mechanism, not a timer race: see tests/lib/offload_check.js): the work left the loop thread.
// Mutation: removing any one codec's DYN_OFFLOAD_CPU makes its "loop ticks" check red (the promise is already settled at return).
// Archives (Tar*/Zip*) build JS objects per entry and stay unmarked. Inputs are 16 MiB of text.
import { suite } from "../lib/kit.js";
import { same, loopTicks, text, equalValue } from "../lib/offload_check.js";
import * as C from "dyna:compress";
import { offload } from "dyna:async";

const t = suite("compress.offload");
const IN = text(16 << 20);
const big = { maxOutputBytes: 64 << 20 };

const CODECS = [
    ["gzip", C.gzip, [IN], "gunzip", (b) => C.gunzip(b, big)],
    ["zstd", C.zstd, [IN, { level: 3 }], "unzstd", (b) => C.unzstd(b, big)],
    ["brotli", C.brotli, [IN, { level: 5 }], "unbrotli", (b) => C.unbrotli(b, big)],
    ["snappy", C.snappy, [IN], "unsnappy", (b) => C.unsnappy(b, big)],
    ["lz4Compress", C.lz4Compress, [IN], "lz4Decompress", (b) => C.lz4Decompress(b, big)],
    ["lz4Frame", C.lz4Frame, [IN], "lz4Unframe", (b) => C.lz4Unframe(b, big)],
    ["deflate", C.deflate, [IN], "inflate", (b) => C.inflate(b, big)],
];
const DEC = { gunzip: C.gunzip, unzstd: C.unzstd, unbrotli: C.unbrotli, unsnappy: C.unsnappy, lz4Decompress: C.lz4Decompress, lz4Unframe: C.lz4Unframe, inflate: C.inflate };

for (const [name, enc, args, dname, dec] of CODECS) {
    t.test(`${name} / ${dname}: offloaded output is byte-identical and round-trips`, async (a) => {
        const packed = await same(a, name, enc, args);
        a.ok(equalValue(dec(packed), IN), `${name}: decodes back to the input`);
        const unpacked = await same(a, dname, DEC[dname], [packed, big]);
        a.ok(equalValue(unpacked, IN), `${dname}: offloaded decode is the input`);
        if (dname !== "lz4Decompress" && dname !== "inflate") {
            const s = await same(a, dname + " asString", DEC[dname], [packed, { ...big, asString: true }]);
            a.eq(typeof s, "string", `${dname}: asString still applies under offload`);
        }
    });
    t.test(`${name} / ${dname}: loop ticks while the work runs off-thread`, async (a) => {
        await loopTicks(a, name, enc, args);
        await loopTicks(a, dname, DEC[dname], [enc(...args), big]);
    });
}

t.test("deflate/inflate zlib format offloads the same", async (a) => {
    const z = await same(a, "deflate zlib", C.deflate, [IN, { format: "zlib" }]);
    await same(a, "inflate zlib", C.inflate, [z, { ...big, format: "zlib" }]);
});

t.test("the input buffer cannot be detached while the offloaded call runs, and the result is of the bytes at call time", async (a) => {
    const want = C.gzip(IN);
    const u = IN.slice();
    const p = offload(C.gzip, u);
    a.throws(() => u.buffer.transfer(), TypeError, "transfer refused while in use");
    u.fill(0);
    a.ok(equalValue(await p, want), "result is the gzip of the bytes at call time");
});

t.test("decoder errors reject with the direct call's error class and message", async (a) => {
    const junk = new Uint8Array(64).fill(7);
    for (const [name, fn] of Object.entries(DEC)) {
        let direct;
        try { fn(junk); } catch (e) { direct = e; }
        if (!direct) continue;
        const e = await a.rejects(offload(fn, junk), direct.constructor, `${name}: rejects`);
        a.eq(e && e.message, direct.message, `${name}: same message`);
    }
});

await t.run();
