// flags: --std
// covers: c:src/modules/libc/libc-blob.c
// Blob reads under setNativeMemoryLimit: arrayBuffer()/bytes() results and stream() chunks are allocated by the
// tracked module allocator and must leave the native ledger when handed to JS (dyn_nat_ab_take), because the engine
// frees them with the plain allocator. Before the fix each read stayed on the ledger forever: nativeSize climbed by
// the read size per call and a later read threw a false out-of-memory under a limit far above the live data.
// ORACLE: arithmetic. One 16 MiB Blob is live; 24 dropped 16 MiB reads (384 MiB) must fit a 96 MiB limit.
// CONTROL: the limit is armed (nativeLimit reports it), so an untracked read would not prove anything.
import { suite } from "../lib/kit.js";
import { memoryUsage, setNativeMemoryLimit } from "dyna:sys";
import * as std from "std";

const t = suite("test_blob_native_limit");
const MiB = 1 << 20;
setNativeMemoryLimit(96 * MiB);
const blob = new Blob([new Uint8Array(16 * MiB).fill(7)]);

t.test("native limit is armed", ({ eq }) => {
    eq(memoryUsage().nativeLimit, 96 * MiB, "nativeLimit");
});

t.test("repeated arrayBuffer()/bytes() under a limit: no false OOM, flat ledger", async ({ eq, ok }) => {
    const n0 = memoryUsage().nativeSize;
    let total = 0;
    for (let i = 0; i < 12; i++) {
        total += (await blob.arrayBuffer()).byteLength;
        total += (await blob.slice(1).bytes()).length;
        std.gc();
    }
    eq(total, 12 * (32 * MiB - 1), "all reads completed");
    ok(memoryUsage().nativeSize - n0 < MiB, "ledger flat: " + n0 + " -> " + memoryUsage().nativeSize);
});

t.test("repeated stream() reads under a limit: no false OOM, flat ledger", async ({ eq, ok }) => {
    const n0 = memoryUsage().nativeSize;
    let total = 0;
    for (let i = 0; i < 8; i++) {
        const r = blob.stream().getReader();
        for (;;) {
            const { done, value } = await r.read();
            if (done) break;
            total += value.length;
        }
        std.gc();
    }
    eq(total, 8 * 16 * MiB, "all chunks read");
    ok(memoryUsage().nativeSize - n0 < MiB, "ledger flat: " + n0 + " -> " + memoryUsage().nativeSize);
});

await t.run();
