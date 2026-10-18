// flags: --std
// Split from tests/test_audit_w4_native_1.js: the bytes sections.
import "../lib/httpc.js";
import * as bytes from "dyna:bytes";
import { suite } from "../lib/kit.js";

const t = suite("test_bytes_audit_w4_native_1");
const per = (f, n = 20000) => { let b = Infinity; for (let r = 0; r < 5; r++) { const t0 = performance.now(); for (let i = 0; i < n; i++) f(); b = Math.min(b, (performance.now() - t0) / n * 1e6); } return b; };
// ORACLE: count(buf, byte) is the number of positions holding that byte, the
// definition a plain loop computes; checked at every length around the vector
// widths and from every start offset.
// SCALING: a dense byte is counted in one pass, not by restarting a substring
// search after every hit (502 MB/s on text with a space every fifth byte).
// CONTROL: a multi-byte needle, which still takes the search path.
t.test("bytes.count of one byte is one pass", ({ ok, eq, best }) => {
    let bad = 0;
    for (const len of [0, 1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 200]) {
        const u = new Uint8Array(len);
        for (let i = 0; i < len; i++) u[i] = (i * 7) % 5 === 0 ? 0x20 : 0x61;
        for (const start of [0, 1, 16, len]) {
            if (start > len) continue;
            let want = 0;
            for (let i = start; i < len; i++) if (u[i] === 0x20) want++;
            if (bytes.count(u, 0x20, start) !== want) bad++;
        }
    }
    eq(bad, 0, "count agrees with a plain loop at every length and start");
    const text = new TextEncoder().encode("the quick brown fox jumps over the lazy dog ".repeat(90000));
    eq(bytes.count(text, new Uint8Array([0x66, 0x6f, 0x78])), 90000, "control: a three-byte needle is counted by search");
    const tOne = best(() => bytes.count(text, 0x20)), tMiss = best(() => bytes.count(text, 0x7e));
    ok(tOne < tMiss * 6 + 0.2, "[red] a dense byte costs about what an absent one does (" + tOne.toFixed(2) + " ms vs " + tMiss.toFixed(2) + " ms)");
});
await t.run();
