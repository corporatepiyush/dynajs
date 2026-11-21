// flags: --std
// covers: os.openAsBlob
// v10b-27: os.openAsBlob keeps the descriptor it opened to snapshot the file (fstat identity, size, mtime) as the File's cached
// read descriptor, inside the same per-process cap (64) that read-time descriptors use, so the first read costs stat(path) + pread
// instead of stat + open + fstat + pread (open is the expensive call: path walk plus the per-open security checks). Every read still
// stat()s the path, so a replaced or modified file keeps failing with NotReadableError (pinned in test_os_open_as_blob_lazy.js).
// Files past the cap close their descriptor at open time and reopen per read as before.
import { suite } from "../lib/kit.js";
import * as os from "os";
import * as std from "std";

const t = suite("open_as_blob_fd");
const dir = std.getenv("TMPDIR") || ".";
let seq = 0;
const byteAt = (i, seed) => (i * 131 + (i >>> 7) * 17 + seed) & 255;
function makeFile(n, seed) {
    const path = `${dir}/oabfd_${os.getpid()}_${seq++}.bin`;
    const f = std.open(path, "wb");
    const b = new Uint8Array(n);
    for (let i = 0; i < n; i++) b[i] = byteAt(i, seed);
    f.write(b.buffer, 0, n);
    f.close();
    return path;
}
const fds = () => os.readdir("/dev/fd")[0].length;

t.test("the descriptor opened by openAsBlob serves the first read: reading opens nothing", async (a) => {
    const path = makeFile(4096, 1);
    try {
        std.gc();
        const f0 = fds();
        let b = os.openAsBlob(path);
        const f1 = fds();
        const v = new Uint8Array(await b.arrayBuffer());
        const f2 = fds();
        let bad = 0;
        for (let i = 0; i < v.length; i++) if (v[i] !== byteAt(i, 1)) bad++;
        a.eq(v.length, 4096);
        a.eq(bad, 0);
        a.eq(f1 - f0, 1, "openAsBlob holds one descriptor");
        a.eq(f2, f1, "the read reused it instead of opening the file again");
        b = null;
        std.gc();
        a.eq(fds(), f0, "the descriptor closes when the File is freed");
    } finally {
        os.remove(path);
    }
});

t.test("300 unread Files hold at most 64 descriptors, all read correctly, and release them", async (a) => {
    const path = makeFile(9000, 2);
    try {
        std.gc();
        const f0 = fds();
        let files = [];
        for (let i = 0; i < 300; i++) files.push(os.openAsBlob(path));
        const held = fds() - f0;
        a.ok(held <= 64, `unread Files hold ${held} descriptors (cap 64)`);
        let bad = 0;
        for (let i = 0; i < files.length; i++) {
            const off = (i * 29) % 8990;
            const v = await files[i].slice(off, off + 7).bytes();
            for (let k = 0; k < 7; k++) if (v[k] !== byteAt(off + k, 2)) bad++;
        }
        a.eq(bad, 0);
        a.ok(fds() - f0 <= 64, `after reads: ${fds() - f0} descriptors`);
        files = null;
        std.gc();
        a.eq(fds(), f0, "every descriptor closes when the Files are freed");
        const again = os.openAsBlob(path);
        a.eq(fds() - f0, 1, "a File opened after the release holds a descriptor again (no leaked cap slots)");
        a.eq((await again.slice(5, 6).bytes())[0], byteAt(5, 2));
    } finally {
        os.remove(path);
    }
});

t.test("an empty file holds no descriptor", async (a) => {
    const path = makeFile(0, 3);
    try {
        std.gc();
        const f0 = fds();
        const b = os.openAsBlob(path);
        a.eq(fds(), f0);
        a.eq((await b.arrayBuffer()).byteLength, 0);
    } finally {
        os.remove(path);
    }
});
await t.run();
