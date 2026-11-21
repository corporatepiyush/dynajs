// covers: dyna:file.sniffType
// covers: c:src/core/fsops/fsops-copy.c
// After the magic table, sniffType calls bytes binary when a NUL appears in the
// first 512 bytes (memchr over min(n, 512)). The oracle is that rule written in
// JS: every NUL position around the window edge and every short length.
import { sniffType, Path, makeTempDir, writeFile, removeAll } from "dyna:file";
import { suite } from "../lib/kit.js";

const t = suite("test_file_sniff_nul_window");
const text = (n) => {
    const b = new Uint8Array(n);
    for (let i = 0; i < n; i++) b[i] = 0x61 + (i % 26);
    return b;
};
const oracle = (b) => (b.length === 0 || b.subarray(0, 512).includes(0) ? "application/octet-stream" : "text/plain");

t.test("a NUL decides binary only inside the first 512 bytes", ({ eq }) => {
    let bad = 0;
    for (const n of [600, 512, 513, 1024]) {
        for (const pos of [-1, 0, 1, 7, 8, 15, 16, 31, 32, 255, 256, 500, 510, 511, 512, 513, 599]) {
            if (pos >= n) continue;
            const b = text(n);
            if (pos >= 0) b[pos] = 0;
            const got = sniffType(b), exp = oracle(b);
            if (got !== exp && bad++ < 5) eq(got, exp, `n=${n} nul@${pos}`);
        }
    }
    eq(bad, 0, "mismatches");
});

t.test("every short length with and without a trailing NUL", ({ eq }) => {
    let bad = 0;
    for (let n = 0; n <= 70; n++) {
        for (const nul of [false, true]) {
            if (nul && n === 0) continue;
            const b = text(n);
            if (nul) b[n - 1] = 0;
            const got = sniffType(b), exp = oracle(b);
            if (got !== exp && bad++ < 5) eq(got, exp, `n=${n} nul=${nul}`);
        }
    }
    eq(bad, 0, "mismatches");
});

t.test("a file path reads the same window", ({ eq }) => {
    const d = makeTempDir("sniffnul_");
    try {
        const a = text(700);
        a[511] = 0;
        const b = text(700);
        b[512] = 0;
        writeFile(new Path(String(d) + "/a"), a);
        writeFile(new Path(String(d) + "/b"), b);
        eq(sniffType(new Path(String(d) + "/a")), "application/octet-stream", "NUL at 511");
        eq(sniffType(new Path(String(d) + "/b")), "text/plain", "NUL at 512");
    } finally {
        removeAll(d);
    }
});

await t.run();
