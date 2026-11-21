// flags: --std
// covers: os.openAsBlob
// os.openAsBlob returns a File backed by the file itself: nothing is read or mapped at open time, every read (slice bytes, text(),
// arrayBuffer(), stream() chunks, FileReader) preads just the bytes it needs from the path resolved at open time. Streaming a large
// file therefore keeps peak RSS near one 64 KiB chunk instead of the file size (the old mmap made every streamed page resident: a
// 46 MB file peaked at 51 MB). The File API snapshot rule holds: when the file's identity, size or mtime differ from the snapshot
// taken at open, reads fail with a NotReadableError DOMException (stream() errors, text() rejects, FileReader fires error) instead of
// returning the new bytes (or, with the old mmap, SIGBUS on truncation).
import { suite } from "../lib/kit.js";
import { memoryUsage } from "dyna:sys";
import * as os from "os";
import * as std from "std";

const t = suite("open_as_blob_lazy");
const dir = std.getenv("TMPDIR") || ".";
let seq = 0;
const byteAt = (i, seed) => (i * 31 + (i >>> 16) * 101 + seed) & 255;
function makeFile(n, seed) {
    const path = `${dir}/oab_${os.getpid()}_${seq++}.bin`;
    const f = std.open(path, "wb");
    const chunk = new Uint8Array(1 << 20);
    for (let off = 0; off < n; off += chunk.length) {
        const m = Math.min(chunk.length, n - off);
        const hi = ((off >>> 16) * 101 + seed) & 255;
        for (let i = 0; i < m; i++) chunk[i] = (off + i) * 31 + hi + (((off + i) >>> 16) - (off >>> 16)) * 101 & 255;
        f.write(chunk.buffer, 0, m);
    }
    f.close();
    return path;
}

t.test("streaming a 48 MiB file through one BYOB buffer keeps peak RSS flat (allocator-independent: no per-chunk buffers for a sanitizer to quarantine)", async (a) => {
    const N = 48 << 20, path = makeFile(N, 3);
    try {
        std.gc();
        const before = memoryUsage().peakRss;
        const b = os.openAsBlob(path);
        const r = b.stream().getReader({ mode: "byob" });
        let total = 0, bad = 0, view = new Uint8Array(1 << 16);
        for (;;) {
            const { value, done } = await r.read(view);
            if (done) break;
            if (value[0] !== byteAt(total, 3)) bad++;
            total += value.length;
            view = new Uint8Array(value.buffer);
        }
        const grew = memoryUsage().peakRss - before;
        a.eq(total, N);
        a.eq(bad, 0);
        a.ok(grew < (16 << 20), `peak RSS grew ${(grew / 1048576).toFixed(1)} MB while streaming 48 MB`);
    } finally {
        os.remove(path);
    }
});

t.test("default-reader stream chunks carry the right bytes", async (a) => {
    const path = makeFile(1 << 20, 4);
    try {
        const r = os.openAsBlob(path).stream().getReader();
        let total = 0, bad = 0, chunks = 0;
        for (;;) {
            const { value, done } = await r.read();
            if (done) break;
            for (let i = 0; i < value.length; i += 4093) if (value[i] !== byteAt(total + i, 4)) bad++;
            total += value.length;
            chunks++;
        }
        a.eq([total, bad, chunks].join(","), [1 << 20, 0, 16].join(","));
    } finally {
        os.remove(path);
    }
});

t.test("slices, text, arrayBuffer, bytes and BYOB read the right bytes", async (a) => {
    const path = makeFile(300000, 7);
    try {
        const b = os.openAsBlob(path, { type: "text/plain" });
        const s = await b.slice(123456, 123466).bytes();
        a.eq(Array.from(s).join(","), Array.from({ length: 10 }, (_, i) => byteAt(123456 + i, 7)).join(","));
        a.eq((await b.arrayBuffer()).byteLength, 300000);
        a.eq((await b.text()).length > 0, true);
        const mixed = new Blob(["ab", b.slice(5, 9), "cd"]);
        a.eq(Array.from(await mixed.bytes()).join(","), [97, 98, byteAt(5, 7), byteAt(6, 7), byteAt(7, 7), byteAt(8, 7), 99, 100].join(","));
        const rd = b.slice(1000, 1100).stream().getReader({ mode: "byob" });
        const { value } = await rd.read(new Uint8Array(64));
        a.eq(value[0], byteAt(1000, 7));
    } finally {
        os.remove(path);
    }
});

t.test("a file modified after openAsBlob fails reads with NotReadableError", async (a) => {
    const path = makeFile(200000, 1);
    const b = os.openAsBlob(path);
    const f = std.open(path, "r+b");
    f.seek(0, std.SEEK_END);
    f.write(new Uint8Array(10).buffer, 0, 10);
    f.close();
    try {
        let e1 = null;
        await b.text().catch((e) => { e1 = e; });
        a.ok(e1 instanceof DOMException && e1.name === "NotReadableError", `text(): ${e1}`);
        let e2 = null;
        await b.stream().getReader().read().catch((e) => { e2 = e; });
        a.ok(e2 instanceof DOMException && e2.name === "NotReadableError", `stream(): ${e2}`);
        const fr = new FileReader();
        const ev = await new Promise((res) => { fr.onerror = () => res("error"); fr.onload = () => res("load"); fr.readAsArrayBuffer(b); });
        a.eq(ev, "error");
        a.eq(fr.error.name, "NotReadableError");
    } finally {
        os.remove(path);
    }
});

t.test("a file replaced (new inode, same size) after openAsBlob fails reads", async (a) => {
    const path = makeFile(5000, 2);
    const b = os.openAsBlob(path);
    os.remove(path);
    const p2 = makeFile(5000, 2);
    os.rename(p2, path);
    try {
        let e = null;
        await b.bytes().catch((x) => { e = x; });
        a.ok(e && e.name === "NotReadableError", `bytes(): ${e}`);
    } finally {
        os.remove(path);
    }
});

t.test("the path is resolved at open: a later chdir does not change what is read", async (a) => {
    const path = makeFile(1000, 5);
    const cwd = os.getcwd()[0];
    const base = path.slice(path.lastIndexOf("/") + 1), d = path.slice(0, path.lastIndexOf("/"));
    os.chdir(d);
    let b;
    try {
        b = os.openAsBlob(base);
    } finally {
        os.chdir(cwd);
    }
    try {
        os.chdir("/");
        const v = await b.slice(10, 11).bytes();
        a.eq(v[0], byteAt(10, 5));
    } finally {
        os.chdir(cwd);
        os.remove(path);
    }
});

t.test("small reads use a per-thread 64 KiB block cache: boundary straddles read the right bytes, and a modification still fails the next read", async (a) => {
    const path = makeFile(200000, 9);
    try {
        const b = os.openAsBlob(path);
        let bad = 0;
        for (const off of [0, 65530, 65535, 65536, 131070, 131072 - 3, 199990]) {
            const v = await b.slice(off, off + 10).bytes();
            for (let i = 0; i < v.length; i++) if (v[i] !== byteAt(off + i, 9)) bad++;
        }
        for (let i = 0; i < 2000; i++) {
            const off = (i * 997) % 199000;
            const v = await b.slice(off, off + 64).bytes();
            if (v[0] !== byteAt(off, 9) || v[63] !== byteAt(off + 63, 9)) bad++;
        }
        a.eq(bad, 0);
        const g = std.open(path, "r+b");
        g.seek(100, std.SEEK_SET);
        g.write(new Uint8Array([1, 2, 3]).buffer, 0, 3);
        g.close();
        os.utimes(path, Date.now(), Date.now() + 5000);
        let e = null;
        await b.slice(10, 20).bytes().catch((x) => { e = x; });
        a.ok(e && e.name === "NotReadableError", `cached block after modification: ${e}`);
    } finally {
        os.remove(path);
    }
});

t.test("the shared small-read block is keyed by file: interleaved small reads of two files at the same offsets return each file's own bytes", async (a) => {
    const p1 = makeFile(150000, 3), p2 = makeFile(150000, 4);
    try {
        const b1 = os.openAsBlob(p1), b2 = os.openAsBlob(p2);
        let bad = 0;
        for (let i = 0; i < 400; i++) {
            const off = (i * 613) % 149000, b = i & 1 ? b2 : b1, seed = i & 1 ? 4 : 3;
            const v = await b.slice(off, off + 32).bytes();
            if (v[0] !== byteAt(off, seed) || v[31] !== byteAt(off + 31, seed)) bad++;
        }
        a.eq(bad, 0);
    } finally {
        os.remove(p1);
        os.remove(p2);
    }
});

t.test("more read blobs than the cached-fd cap still read correctly and release their fds", async (a) => {
    const fds = () => os.readdir("/dev/fd")[0].length;
    const path = makeFile(70000, 6);
    std.gc();
    const f0 = fds();
    try {
        let blobs = [];
        for (let i = 0; i < 150; i++) blobs.push(os.openAsBlob(path));
        let bad = 0;
        for (let i = 0; i < blobs.length; i++) {
            const v = await blobs[i].slice(i * 7, i * 7 + 3).bytes();
            if (v[0] !== byteAt(i * 7, 6)) bad++;
        }
        a.eq(bad, 0);
        a.ok(fds() - f0 <= 64, `cached fds are capped: ${fds() - f0}`);
        blobs = null;
        std.gc();
        a.eq(fds(), f0, "every cached fd closes when its blob store is freed");
        const again = os.openAsBlob(path);
        a.eq((await again.slice(5, 6).bytes())[0], byteAt(5, 6));
        a.eq(fds() - f0, 1, "after the cap was exceeded and released, a new File caches its fd again");
    } finally {
        os.remove(path);
    }
});

t.test("the path is resolved from the open file: relative paths survive chdir, a symlink keeps its target", async (a) => {
    const sub = `${dir}/oab_dir_${os.getpid()}`;
    os.mkdir(sub);
    const p1 = makeFile(5000, 3), p2 = makeFile(5000, 9);
    const want1 = new Uint8Array(await os.openAsBlob(p1).arrayBuffer());
    const want2 = new Uint8Array(await os.openAsBlob(p2).arrayBuffer());
    const link = `${sub}/ln.bin`;
    os.symlink(p1, link);
    const [cwd] = os.getcwd();
    try {
        os.chdir(sub);
        const rel = os.openAsBlob("ln.bin");
        os.chdir("/");
        a.eq(rel.size, 5000);
        const got = new Uint8Array(await rel.arrayBuffer());
        a.eq(got.length === want1.length && got.every((v, i) => v === want1[i]), true);
        const viaLink = os.openAsBlob(link);
        os.remove(link);
        os.symlink(p2, link);
        const again = new Uint8Array(await viaLink.arrayBuffer());
        a.eq(again.every((v, i) => v === want1[i]), true);
        a.eq(want1.some((v, i) => v !== want2[i]), true);
    } finally {
        os.chdir(cwd);
        os.remove(link);
        os.remove(sub);
        os.remove(p1);
        os.remove(p2);
    }
});

await t.run();
