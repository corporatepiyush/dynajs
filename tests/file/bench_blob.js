// covers: c:src/modules/libc/libc-blob.c
// Portable Blob benchmark: the same file runs under dynajs, node and bun (rule 11 gap table). One scenario
// per process so peak RSS (measured outside with /usr/bin/time -l) belongs to that scenario alone.
//   dynajs --std tests/file/bench_blob.js MODE N     node tests/file/bench_blob.js MODE N
// Modes: idle (baseline RSS), small N (N live one-byte Blobs: bytes/Blob), slice N (N live 1 MiB slices of one
// 64 MiB Blob: sharing, no copy), build1g (1024 x 1 MiB Uint8Array parts -> one 1 GiB Blob), rope1g (1024 x
// 1 MiB Blob parts -> one Blob, shared), text N (text() of an N MiB ASCII Blob), ab N (arrayBuffer() of N MiB),
// stream N (read stream() of N MiB to the end), ctor N (N constructions of new Blob(["abc", u8])),
// slops N (N slices of a 3-store rope), churn N (N rounds of build+read+drop 64 MiB: growth per round).
// Prints one line: mode n ms result [mem...]; mem is process RSS where the runtime exposes it.
const isDyna = typeof scriptArgs !== "undefined";
const args = isDyna ? scriptArgs.slice(1) : process.argv.slice(2);
const mode = args[0] || "idle", N = +(args[1] || 0);
let sys = null, std = null;
if (isDyna) {
    sys = await import("dyna:sys");
    std = await import("std");
}
const now = () => performance.now();
function mem() {
    if (isDyna) {
        const m = sys.memoryUsage();
        return "malloc=" + m.mallocSize + " peakRss=" + m.peakRss;
    }
    const m = process.memoryUsage();
    return "rss=" + m.rss + " external=" + m.external + " ab=" + (m.arrayBuffers || 0);
}
function gc() {
    if (isDyna) std.gc();
    else if (typeof Bun !== "undefined") Bun.gc(true);
    else if (globalThis.gc) globalThis.gc();
}
const MiB = 1 << 20;
let keep, out = "";
const t0 = now();
switch (mode) {
    case "idle":
        break;
    case "small": {
        keep = new Array(N);
        for (let i = 0; i < N; i++) keep[i] = new Blob(["x"]);
        out = keep.length;
        break;
    }
    case "slice": {
        const base = new Blob([new Uint8Array(64 * MiB)]);
        keep = new Array(N);
        for (let i = 0; i < N; i++) keep[i] = base.slice((i * 4096) % (63 * MiB), (i * 4096) % (63 * MiB) + MiB);
        out = keep.reduce((a, b) => a + b.size, 0);
        break;
    }
    case "build1g": {
        let parts = [];
        for (let i = 0; i < 1024; i++) parts.push(new Uint8Array(MiB).fill(i & 255));
        keep = new Blob(parts);
        parts = null;
        gc();
        out = keep.size;
        break;
    }
    case "rope1g": {
        let parts = [];
        for (let i = 0; i < 1024; i++) parts.push(new Blob([new Uint8Array(MiB).fill(i & 255)]));
        const t1 = now();
        keep = new Blob(parts);
        out = keep.size + " concat_ms=" + (now() - t1).toFixed(3);
        break;
    }
    case "text": {
        const b = new Blob(["a".repeat(N * MiB)]);
        const t1 = now();
        const s = await b.text();
        out = s.length + " read_ms=" + (now() - t1).toFixed(2);
        break;
    }
    case "ab": {
        const b = new Blob([new Uint8Array(N * MiB)]);
        const t1 = now();
        const a = await b.arrayBuffer();
        out = a.byteLength + " read_ms=" + (now() - t1).toFixed(2);
        break;
    }
    case "stream": {
        const b = new Blob([new Uint8Array(N * MiB)]);
        const t1 = now();
        const r = b.stream().getReader();
        let total = 0, chunks = 0;
        for (;;) {
            const { done, value } = await r.read();
            if (done) break;
            total += value.length;
            chunks++;
        }
        out = total + " chunks=" + chunks + " read_ms=" + (now() - t1).toFixed(2);
        break;
    }
    case "ctor": {
        const u = new Uint8Array([1, 2, 3]);
        let s = 0;
        for (let i = 0; i < N; i++) s += new Blob(["abc", u]).size;
        out = s;
        break;
    }
    case "slops": {
        const rope = new Blob([new Blob([new Uint8Array(4096)]), "mid", new Blob([new Uint8Array(4096)])]);
        let s = 0;
        for (let i = 0; i < N; i++) s += rope.slice(i & 1023, 4000 + (i & 1023)).size;
        out = s;
        break;
    }
    case "churn": {
        const line = [];
        for (let r = 0; r < N; r++) {
            let b = new Blob([new Uint8Array(64 * MiB)]);
            const a = await b.slice(MiB).arrayBuffer();
            b = null;
            gc();
            line.push(a.byteLength > 0 ? mem() : "");
        }
        out = "\n  " + line.join("\n  ");
        break;
    }
}
const ms = now() - t0;
console.log(mode + " " + N + " " + ms.toFixed(2) + " " + out + " | " + mem());
