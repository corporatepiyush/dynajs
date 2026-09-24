// flags: --std
/* test_file_bytes.js — dyna:file BYTE paths and the glob
 * grammar/parity pins (runtime half).
 *
 * The string paths (read/readLine/readAll) decode UTF-8; the byte paths
 * (readFile {bytes}, readBytes, FileReader.readInto/readBytes) move raw
 * bytes. The two families are pinned separately here because a regression
 * that quietly routes a byte read through a string shows up as U+FFFD
 * corruption — the exact defect class that motivated the byte paths.
 *
 * readInto's boundary matrix is driven from a 4-byte reader buffer so the
 * "loop fills" arm runs on every case; the default 128 KiB buffer would
 * hide it. */
import * as std from "std";
import {
    Path, File, FileReader, readFile, readBytes, writeFile, makeDir,
    removeAll, remove, glob, Glob, FileLock, Watcher, symlink,
} from "dyna:file";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; throw new Error("assertion failed: " + m); } }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL(no throw): " + m); }
    catch (e) {
        if (!re.test(String(e.message))) { fails++; print(`FAIL(${e.message}): ` + m); }
    }
}

const dir = new Path(Path.temp(), "dj_fbytes." + (Date.now() % 10000000));
makeDir(dir, { recursive: true });
const bin = new Path(dir, "bin.dat");

function fresh(data, bufsize) {
    writeFile(bin, data);
    return new FileReader(bin, { bufferSize: bufsize || 4 });
}
function eq(a, b, m) {
    assert(a.length === b.length && a.every((v, i) => v === b[i]),
           m + ` (${JSON.stringify(Array.from(a))} vs ${JSON.stringify(Array.from(b))})`);
}

/* ---: readFile {bytes} and readBytes(path): invalid UTF-8 round-trips --- */
{
    const raw = new Uint8Array([0x00, 0x01, 0xFE, 0xFF, 0x00, 0x0D, 0x0A, 0xC8, 0x64, 0x80, 0xFF]);
    writeFile(bin, raw);
    const viaFlag = readFile(bin, { bytes: true });
    const viaAlias = readBytes(bin);
    const viaHandle = new File(bin).readBytes();
    eq(viaFlag, raw, "readFile {bytes:true} round-trips invalid UTF-8");
    eq(viaAlias, raw, "readBytes(path) round-trips invalid UTF-8");
    eq(viaHandle, raw, "File.readBytes() round-trips invalid UTF-8");
    /* the string path provably cannot: the invalid bytes arrive as U+FFFD
     * (the exact count is the UTF-8 decoder's business, not this module's;
     * the byte paths' contract is that they need no decoder at all) */
    const text = readFile(bin);
    let fffd = 0;
    for (const ch of text) if (ch === "\uFFFD") fffd++;
    assert(fffd >= 3, "the string path mangles the invalid bytes to U+FFFD, the byte paths none");

    /* empty file: empty u8, not an exception, not undefined */
    const empty = new Path(dir, "empty.bin");
    writeFile(empty, "");
    assert(readBytes(empty).length === 0, "readBytes of a 0-byte file is an empty Uint8Array");
    assert(readFile(empty, { bytes: true }).length === 0, "readFile {bytes} of a 0-byte file is empty");
    /* a 1 MiB+ payload exercises the >fill-buffer read in the free form too */
    const big = new Uint8Array((1 << 20) + 13);
    for (let i = 0; i < big.length; i++) big[i] = (i * 7 + 3) & 0xff;
    const bigPath = new Path(dir, "big.bin");
    writeFile(bigPath, big);
    eq(readBytes(bigPath), big, "readBytes over 1 MiB is byte-exact");
    remove(bigPath);
}

/* ---: readInto boundary matrix (reader bufferSize = 4) --- */
{
    /* exact multiple of the fill buffer: 12 bytes = 3 fills */
    const raw = new Uint8Array(12);
    for (let i = 0; i < 12; i++) raw[i] = (i + 1) * 10;
    const r = fresh(raw);
    const out = new Uint8Array(12);
    assert(r.readInto(out) === 12, "readInto fills a buffer exactly 3x the read buffer");
    eq(out, raw, "readInto content exact across fills");
    assert(r.readInto(out) === 0, "readInto returns 0 at EOF");
    assert(r.readInto(out) === 0, "readInto keeps returning 0 at EOF");
    r.close();

    /* buf SMALLER than the fill buffer: a partial fill is left readable */
    const r2 = fresh(raw);
    const head = new Uint8Array(3);
    assert(r2.readInto(head) === 3, "readInto into a smaller buffer returns its size");
    eq(head, raw.subarray(0, 3), "small readInto content");
    const tail = new Uint8Array(9);
    assert(r2.readInto(tail) === 9, "the unconsumed remainder survives the small read");
    eq(tail, raw.subarray(3), "remainder content");
    assert(r2.readInto(tail) === 0, "EOF after exact consumption");
    r2.close();

    /* buf LARGER than the fill buffer, ending mid-fill: 4-byte reader, 5-byte file */
    const five = new Uint8Array([1, 2, 3, 4, 5]);
    const r3 = fresh(five);
    const six = new Uint8Array(6);
    assert(r3.readInto(six) === 5, "readInto returns the byte count, not the buffer size, at a short EOF");
    eq(six.subarray(0, 5), five, "short-EOF content");
    assert(six[5] === 0, "no garbage past the returned count");
    r3.close();

    /* many small reads across many fills */
    const r4 = fresh(raw);
    let acc = [], got = 0, one;
    while ((got = r4.readInto(one = new Uint8Array(2))) > 0) {
        assert(got === 1 || got === 2, "small reads return 1 or 2");
        for (let i = 0; i < got; i++) acc.push(one[i]);
    }
    assert(acc.length === 12 && acc.every((v, i) => v === raw[i]), "2-byte reads reassemble the file");
    r4.close();

    /* 0-byte file and 0-byte buffer */
    const r5 = fresh(new Uint8Array(0));
    assert(r5.readInto(new Uint8Array(8)) === 0, "readInto on a 0-byte file returns 0 (EOF)");
    r5.close();
    const r6 = fresh(raw);
    assert(r6.readInto(new Uint8Array(0)) === 0, "a 0-length buffer reports 0 without reading");
    const probe = new Uint8Array(4);
    assert(r6.readInto(probe) === 4 && probe[0] === raw[0],
           "a 0-length readInto did not consume file data");
    r6.close();

    /* subviews: offset+length of the VIEW bound the copy, not the whole buffer */
    const r7 = fresh(raw);
    const ab = new ArrayBuffer(32);
    const sub = new Uint8Array(ab, 8, 12);
    assert(r7.readInto(sub) === 12, "readInto into an offset subview");
    const whole = new Uint8Array(ab);
    assert(whole[7] === 0 && whole[8] === 10 && whole[19] === 120 && whole[20] === 0,
           "the view's byte offset was honored on both sides");
    r7.close();

    /* non-uint8 views and DataView: raw bytes land in the backing store */
    const r8 = fresh(raw);
    const ab2 = new ArrayBuffer(16);
    const i32 = new Int32Array(ab2, 0, 3); /* 12 raw bytes */
    assert(r8.readInto(i32) === 12, "readInto into an Int32Array writes raw bytes");
    const dv = new DataView(ab2, 12, 4);
    const r8b = fresh(raw.subarray(0, 2));
    assert(r8b.readInto(dv) === 2, "readInto into a DataView subview");
    const check = new Uint8Array(ab2);
    eq(check.subarray(0, 12), raw, "Int32Array backing bytes are the file's");
    assert(check[12] === raw[0] && check[13] === raw[1], "DataView backing bytes continue at its offset");
    r8.close(); r8b.close();

    /* UTF-8 sequence SPLIT at the caller's buffer edge: by design, bytes */
    const text = "héllo"; /* h 0xC3 0xA9 l l o */
    writeFile(bin, text);
    const rt = fresh(text, 3);
    const a = new Uint8Array(2);
    const b = new Uint8Array(4);
    assert(rt.readInto(a) === 2 && rt.readInto(b) === 4, "split read lengths");
    assert(a[1] === 0xC3 && b[0] === 0xA9, "the 2-byte é arrived SPLIT across reads: readInto is a bytes primitive");
    rt.close();

    /* refusals */
    const r9 = fresh(raw);
    throws(() => r9.readInto(), /requires a Uint8Array/i, "readInto requires a buffer");
    throws(() => r9.readInto("nope"), /TypedArray|ArrayBuffer/i, "readInto refuses a non-view");
    throws(() => r9.readInto(42), /TypedArray|ArrayBuffer/i, "readInto refuses a number");
    r9.close();
    throws(() => r9.readInto(new Uint8Array(4)), /./, "readInto on a closed reader refuses (no UAF)");
    throws(() => r9.readBytes(), /./, "readBytes on a closed reader refuses (no UAF)");
}

/* ---: reader readBytes(n) — the byte twin of read(n) --- */
{
    const raw = new Uint8Array(100);
    for (let i = 0; i < 100; i++) raw[i] = i;
    const r = fresh(raw);
    const head = r.readBytes(7);
    assert(head instanceof Uint8Array && head.length === 7 && head[6] === 6, "readBytes(7)");
    const mid = r.readBytes(0); /* 0 = nothing (explicit), like read(0) */
    assert(mid.length === 0, "readBytes(0) reads nothing");
    const rest = r.readBytes();
    assert(rest.length === 93 && rest[92] === 99, "readBytes() takes the rest");
    assert(r.readBytes(5).length === 0, "readBytes at EOF is empty");
    r.close();

    /* interleaved with the string path: both consume the SAME stream */
    writeFile(bin, "0123456789");
    const r2 = new FileReader(bin, { bufferSize: 4 });
    assert(r2.read(2) === "01", "string read first");
    eq(r2.readBytes(3), new Uint8Array([0x32, 0x33, 0x34]), "byte read continues the same stream");
    assert(r2.readLine() === "56789", "readLine continues after the byte read");
    r2.close();

    /* n larger than the file */
    const r3 = fresh(new Uint8Array([9, 8, 7]));
    assert(r3.readBytes(1000).length === 3, "readBytes(n > file) clamps to the file");
    r3.close();
}

/* ---: glob grammar + option parity, walk vs lexical, pinned --- */
{
    const gdir = new Path(dir, "gl");
    makeDir(new Path(gdir, "sub", "deep"), { recursive: true });
    makeDir(new Path(gdir, ".hiddendir"), { recursive: true });
    writeFile(new Path(gdir, "a.txt"), "x");
    writeFile(new Path(gdir, "b.txt"), "x");
    writeFile(new Path(gdir, ".hidden"), "x");
    writeFile(new Path(gdir, "sub", "c.txt"), "x");
    writeFile(new Path(gdir, "sub", "deep", "d.txt"), "x");
    writeFile(new Path(gdir, ".hiddendir", "e.txt"), "x");

    const names = (arr) => arr.map((p) => String(p)).map((s) => s.split("/").pop()).sort();

    /* walk: ** spans segments, lists dirs, skips dotfiles, matches zero */
    const all = names(glob(new Path(gdir, "**")));
    assert(all.includes("a.txt") && all.includes("d.txt"), "** reaches nested files");
    assert(all.includes("sub") && all.includes("deep"), "a trailing ** also emits directories");
    assert(!all.includes(".hidden") && !all.includes("e.txt"), "** skips dotfile entries");
    const zero = names(glob(new Path(gdir, "sub", "**", "*.txt")));
    assert(zero.includes("c.txt") && zero.includes("d.txt"), "** between segments matches zero segments too");
    const one = names(glob(new Path(gdir, "*.txt")));
    assert(one.join(",") === "a.txt,b.txt", "* stays within one segment");
    assert(!one.includes(".hidden"), "* never matches a leading dot (minimatch rule)");
    assert(names(glob(new Path(gdir, ".*"))).includes(".hidden"), "a dot-leading pattern reaches dotfiles");
    assert(names(glob(new Path(gdir, "[ab].txt"))).join(",") === "a.txt,b.txt", "[ab] class");
    assert(names(glob(new Path(gdir, "[!a].txt"))).join(",") === "b.txt", "[!...] negated class");
    assert(names(glob(new Path(gdir, "?.txt"))).length === 2, "? is one character");
    assert(names(glob(new Path(gdir, "{a,b}.txt"))).length === 0, "brace expansion is NOT supported");
    assert(glob(new Path(gdir, "**"), { cwd: undefined }).length > 0, "cwd: undefined = no option");
    /* pinned parity subtlety: an absolute `dir/**` pattern EMITS the root
     * directory itself (it is the '**'-at-depth-zero match); with {cwd} the
     * root is implied and never emitted, and results come back relative. */
    const cwded = glob("**", { cwd: gdir });
    assert(cwded.every((p) => !String(p).startsWith("/")), "{cwd} results are relative");
    assert(names(cwded).length === all.length - 1, "{cwd} walk = same tree minus the implied root");
    throws(() => glob("**", { followSymlinks: true }), /unknown option "followSymlinks" \(valid: cwd\)/,
           "followSymlinks does not exist and is refused, not silently ignored");

    /* lexical Glob: the whole-path matcher, no walk rules */
    const g = new Glob("**/*.txt");
    assert(g.matches(new Path(gdir, "sub", "c.txt")), "lexical ** spans");
    assert(g.matches(new Path("nowhere/at/all/x.txt")), "lexical match needs no filesystem");
    assert(g.matches(new Path(gdir, "a.txt")), "lexical **/ matches zero segments");
    const star = new Glob("*.txt");
    assert(star.matches(new Path(gdir, "sub", "c.txt")), "LEXICAL * crosses / (the walk's per-segment rule is the walk's)");
    assert(new Glob("*idden").matches(new Path(".hidden")), "lexical has NO leading-dot rule (walk-only), pinned");
    assert(star.filter([new Path(gdir, "a.txt"), new Path("z.json")]).length === 1, "filter uses the same lexical matcher");
    assert(new Glob("plain").hasWildcard === false, "hasWildcard");
    throws(() => g.matches("a/string"), /must be a Path/, "matches stays Path-only");
    throws(() => new Glob(42), /string/, "Glob(pattern) requires a string");

    /* symlinks (guarded: some CI filesystems refuse them): ** emits the link
     * as a leaf but never descends through it */
    try {
        symlink(String(new Path(gdir, "sub")), new Path(gdir, "link"));
        const fulls = glob(new Path(gdir, "**")).map(String);
        assert(fulls.some((s) => s.split("/").pop() === "link"), "the symlink is emitted as a leaf");
        assert(!fulls.some((s) => s.slice(String(gdir).length + 1).startsWith("link/")),
               "** never walks THROUGH a symlinked dir");
    } catch (e) {
        print("  (glob symlink pin skipped: " + e.message + ")");
    }

    removeAll(gdir);
}

/* ---: the remaining bags (Watcher; FileLock valid use) --- */
{
    throws(() => new Watcher(dir, { recursiv: true }).close(),
           /unknown option "recursiv" \(valid: recursive, debounceMs, ignore\)/, "Watcher ctor bag");
    throws(() => new Watcher(dir, { recursive: true, debounce: 5 }).close(),
           /unknown option "debounce"/, "Watcher ctor bag (2nd key)");
    const target = new Path(dir, "locked.txt");
    writeFile(target, "x");
    const l = new FileLock(target, { retry: 0, retryMs: 1 });
    assert(l.withLock(() => "crit") === "crit", "FileLock with known keys still works");
    assert(l.closed, "withLock consumes the lock");
}

removeAll(dir);
print(`test_file_bytes: ${n} assertions passed`);
if (fails) throw new Error(fails + " failures");
