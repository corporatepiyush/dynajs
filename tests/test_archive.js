/* test_archive.js -- tar and zip in dyna:compress (design 22).
 *
 * THE ORACLE IS THE SYSTEM'S OWN tar AND unzip, driven through dyna:sys.Exec.
 * A round trip through my own reader is blind to a self-consistent format
 * mistake -- an archive only I can read is not an archive. So every packer
 * output is handed to the real tool, and every real tool's output is read
 * back here. Where a tool is missing the case is SKIPPED and the skip is
 * printed, because a silently lower assertion count reads as green.
 *
 * The security case is zip-slip and tar-slip, which are the same bug: a
 * member named ../../etc/passwd. It is refused at the PARSE boundary, so a
 * caller who writes entry.name to disk is safe without knowing to check.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_archive.js
 */
import { TarPack, TarList, TarExtract, ZipPack, ZipList, ZipRead } from "dyna:compress";
import { Exec, Which, getEnv } from "dyna:sys";
import { Path, makeTempDir, writeFile, removeAll } from "dyna:file";

/* dyna:file has no SYNCHRONOUS byte read -- readFile returns a string,
 * which mangles an archive. cat(1) through Exec is the byte read here,
 * and the gap is recorded in the plan. */
const readBytes = (p) => Exec("cat", [p], { encoding: "bytes" }).stdout;

let n = 0, fails = 0, skips = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
/* A skip is invisible in "0 failures", and the skipped cases here are the
   FOREIGN-implementation oracles -- the only ones that read a field we wrote
   wrong. Where the tools are supposed to exist, a skip is a failure. */
const REQUIRE_TOOLS = (getEnv("DYNAJS_REQUIRE_TOOLS") === "1");
function skip(why) {
    if (REQUIRE_TOOLS) { assert(false, "REQUIRED tool missing: " + why); return; }
    skips++; print("  SKIP: " + why);
}
const dec = (u8) => new TextDecoder().decode(u8);
const j = (v) => JSON.stringify(v);

/* ------------------------------------------------------------------ tar */

{
    const t = TarPack([
        { name: "a.txt", data: "hello" },
        { name: "dir/" },
        { name: "dir/b.bin", data: new Uint8Array([1, 2, 3]) },
    ]);
    eq(t.length % 512, 0, "a tar archive is a whole number of 512-byte blocks");
    const list = TarList(t);
    eq(list.length, 3, "three entries");
    eq(list[0].name, "a.txt", "the first name");
    eq(list[0].size, 5, "and its size");
    eq(list[1].type, "directory", "a directory entry");
    eq(list[2].name, "dir/b.bin", "a nested name");
    const ex = TarExtract(t);
    eq(dec(ex[0].data), "hello", "the body round-trips");
    eq(Array.from(ex[2].data).join(","), "1,2,3", "including bytes");
    assert(ex[1].data === undefined, "a directory has no body");
}
eq(TarList(TarPack([])).length, 0, "an empty archive lists nothing");
{
    /* A name over 100 bytes needs the ustar prefix field, and one that cannot
     * be split is refused rather than truncated. */
    const deep = "a/".repeat(60) + "f.txt";      /* 125 bytes, splittable */
    eq(TarList(TarPack([{ name: deep, data: "x" }]))[0].name, deep,
       "a 125-byte name survives through the prefix field");
    throws(() => TarPack([{ name: "z".repeat(120), data: "x" }]),
           "an unsplittable long name is refused, not truncated");
}
{
    const big = "y".repeat(5000);
    eq(dec(TarExtract(TarPack([{ name: "big", data: big }]))[0].data), big,
       "a body spanning many blocks");
}

/* THE DIFFERENTIAL: the system tar reads what this writes, and vice versa. */
const tarBin = Which("tar");
if (!tarBin) {
    skip("no tar(1) on this host, so the cross-implementation check cannot run");
} else {
    const dir = String(makeTempDir("archtest"));
    try {
        const arc = dir + "/out.tar";
        writeFile(new Path(arc), TarPack([
            { name: "one.txt", data: "first file\n" },
            { name: "sub/two.txt", data: "second file\n" },
        ]));
        const r = Exec(tarBin, ["-tf", arc]);
        eq(r.code, 0, "the system tar accepts an archive this module wrote");
        eq(r.stdout, "one.txt\nsub/two.txt\n", "and lists exactly its entries");
        const x = Exec(tarBin, ["-xOf", arc, "sub/two.txt"]);
        eq(x.stdout, "second file\n", "and extracts the right bytes");

        /* And the other direction: read what the system tar wrote. */
        writeFile(new Path(dir + "/src.txt"), "made by tar\n");
        const c = Exec(tarBin, ["-cf", dir + "/in.tar", "-C", dir, "src.txt"]);
        eq(c.code, 0, "the system tar writes an archive");
        const entries = TarExtract(readBytes(dir + "/in.tar"));
        /* bsdtar adds an AppleDouble `._src.txt` member, so match EXACTLY. */
        const f = entries.filter((e) => e.name === "src.txt")[0];
        assert(f !== undefined, "which this module finds (" +
               entries.map((e) => e.name).join(",") + ")");
        if (f) eq(dec(f.data), "made by tar\n", "and reads byte for byte");
    } finally {
        removeAll(new Path(dir));
    }
}

/* ------------------------------------------------------------------- zip */

{
    const long = "hello world ".repeat(20);
    const z = ZipPack([
        { name: "a.txt", data: long },
        { name: "b.bin", data: new Uint8Array([9, 9, 9]) },
    ]);
    const list = ZipList(z);
    eq(list.length, 2, "two members");
    eq(list[0].name, "a.txt", "named");
    eq(list[0].size, long.length, "with the uncompressed size");
    eq(list[0].method, "deflate", "a compressible member is deflated");
    assert(list[0].compressedSize < list[0].size, "and is actually smaller");
    eq(list[1].method, "store", "an incompressible one is stored");
    eq(dec(ZipRead(z, "a.txt")), long, "the deflated member round-trips");
    eq(Array.from(ZipRead(z, "b.bin")).join(","), "9,9,9", "and the stored one");
    throws(() => ZipRead(z, "nope.txt"), "a missing member is an error");
}
eq(ZipList(ZipPack([])).length, 0, "an empty zip lists nothing");
eq(dec(ZipRead(ZipPack([{ name: "e", data: "" }], { method: "store" }), "e")), "",
   "an empty member");
{
    const big = "z".repeat(200000);
    eq(dec(ZipRead(ZipPack([{ name: "big", data: big }]), "big")), big,
       "a 200 KB member");
}

/* THE DIFFERENTIAL, again: unzip(1) reads what ZipPack writes. */
const unzipBin = Which("unzip");
if (!unzipBin) {
    skip("no unzip(1) on this host");
} else {
    const dir = String(makeTempDir("ziptest"));
    try {
        const arc = dir + "/out.zip";
        writeFile(new Path(arc), ZipPack([
            { name: "one.txt", data: "compress me ".repeat(30) },
            { name: "two.bin", data: new Uint8Array([0, 1, 2, 255]) },
        ]));
        const t = Exec(unzipBin, ["-t", arc]);
        eq(t.code, 0, "unzip(1) verifies an archive this module wrote (" +
           t.stdout.split("\n")[1] + ")");
        assert(t.stdout.indexOf("No errors") >= 0,
               "with no CRC errors -- the CRCs this writes are the real ones");
        const p = Exec(unzipBin, ["-p", arc, "one.txt"]);
        eq(p.stdout, "compress me ".repeat(30),
           "and the bytes it extracts are the bytes that went in");
    } finally {
        removeAll(new Path(dir));
    }
}
const zipBin = Which("zip");
if (!zipBin) {
    skip("no zip(1) on this host, so reading a foreign archive is unchecked");
} else {
    const dir = String(makeTempDir("ziptest2"));
    try {
        writeFile(new Path(dir + "/src.txt"), "made by zip ".repeat(20));
        const c = Exec(zipBin, ["-q", "-j", dir + "/in.zip", dir + "/src.txt"]);
        eq(c.code, 0, "zip(1) writes an archive");
        const list = ZipList(readBytes(dir + "/in.zip"));
        eq(list.length, 1, "which this module lists");
        eq(list[0].name, "src.txt", "with the right name");
        eq(dec(ZipRead(readBytes(dir + "/in.zip"), "src.txt")),
           "made by zip ".repeat(20),
           "and reads a FOREIGN deflate stream byte for byte");
    } finally {
        removeAll(new Path(dir));
    }
}

/* ------------------------------------ tar-slip and zip-slip are one bug */

{
    const evil = ["../etc/passwd", "/etc/passwd", "a/../../b", "..",
                  "..\\windows", "C:\\x", "a\\b"];
    let caught = 0;
    for (const name of evil) {
        try { TarPack([{ name, data: "x" }]); } catch (e) { caught++; }
    }
    eq(caught, evil.length, "the tar writer refuses every unsafe name");
    let zcaught = 0;
    for (const name of evil) {
        try { ZipPack([{ name, data: "x" }]); } catch (e) { zcaught++; }
    }
    eq(zcaught, evil.length, "and so does the zip writer");
}
{
    /* THE READ SIDE IS THE ONE THAT MATTERS: an archive from elsewhere. The
     * bytes are built by hand because the writer refuses to produce them. */
    const safe = TarPack([{ name: "aaaaaaaaaaaaa", data: "x" }]);
    const evil = new Uint8Array(safe);
    const bad = "../../etc/passwd";
    for (let i = 0; i < 13; i++) evil[i] = bad.charCodeAt(i);
    /* fix the checksum so it fails for the NAME, not for the checksum */
    for (let i = 148; i < 156; i++) evil[i] = 32;
    let sum = 0;
    for (let i = 0; i < 512; i++) sum += evil[i];
    const oct = sum.toString(8).padStart(6, "0");
    for (let i = 0; i < 6; i++) evil[148 + i] = oct.charCodeAt(i);
    evil[154] = 0;
    evil[155] = 32;
    let msg = "";
    try { TarList(evil); } catch (e) { msg = String(e.message); }
    assert(msg.indexOf("not safe") >= 0,
           "a traversing name in a FOREIGN archive is refused by name (" + msg + ")");
    eq(TarList(evil, { allowUnsafeNames: true })[0].name.indexOf(".."), 0,
       "and allowUnsafeNames is the explicit opt-out");
}
{
    /* A FOREIGN archive can carry a symlink whose TARGET escapes while the
       entry NAME is safe: linkname must get the same validation (audit 13.8.2).
       The bytes are built by hand because TarPack never writes symlinks. */
    const t = new Uint8Array(TarPack([{ name: "l", data: "x" }]));
    t[156] = 0x32;                                  /* type '2' = symlink */
    const badlink = "../../x";
    for (let i = 0; i < badlink.length; i++) t[157 + i] = badlink.charCodeAt(i);
    for (let i = 148; i < 156; i++) t[i] = 32;      /* recompute checksum */
    let sum = 0;
    for (let i = 0; i < 512; i++) sum += t[i];
    const oct = sum.toString(8).padStart(6, "0");
    for (let i = 0; i < 6; i++) t[148 + i] = oct.charCodeAt(i);
    t[154] = 0; t[155] = 32;
    let msg = "";
    try { TarList(t); } catch (e) { msg = String(e.message); }
    assert(msg.indexOf("link target is not safe") >= 0,
           "a traversing link TARGET in a foreign archive is refused (" + msg + ")");
    eq(TarList(t, { allowUnsafeNames: true })[0].linkname, badlink,
       "and allowUnsafeNames is the explicit opt-out for link targets too");
}

/* ------------------------------------------------- malformed archives */

throws(() => TarList(new Uint8Array([1, 2, 3])), "garbage is not a tar");
throws(() => ZipList(new Uint8Array([1, 2, 3])), "nor a zip");
throws(() => ZipList(new Uint8Array(100)), "a zip needs an EOCD record");
throws(() => TarList(), "an archive is required");
throws(() => ZipRead(ZipPack([{ name: "a", data: "x" }])), "ZipRead needs a name");
throws(() => ZipPack([{ data: "x" }]), "every entry needs a name");
throws(() => ZipPack("not an array"), "entries must be an array");
throws(() => TarPack([{ name: "a", mode: -1 }]), "a negative mode");
throws(() => ZipPack([{ name: "a", data: "x" }], { method: "lzma" }),
       "an unsupported method is refused");
{
    /* A member whose CRC does not match the bytes must be an error, not a
     * warning: the CRC is the format's own integrity check. */
    const z = ZipPack([{ name: "a.txt", data: "the quick brown fox " .repeat(10) }]);
    const bad = new Uint8Array(z);
    /* the central directory's CRC is at a fixed offset from its signature */
    let cd = -1;
    for (let i = bad.length - 22; i >= 0; i--)
        if (bad[i] === 0x50 && bad[i + 1] === 0x4b && bad[i + 2] === 0x01
            && bad[i + 3] === 0x02) { cd = i; break; }
    assert(cd > 0, "the central directory is where the format says");
    bad[cd + 16] ^= 0xff;
    let msg = "";
    try { ZipRead(bad, "a.txt"); } catch (e) { msg = String(e.message); }
    assert(msg.length > 0, "a corrupted CRC is refused (" + msg + ")");
}
{
    /* Two headers naming different files is a real attack: one name for a
     * scanner, another for the extractor. */
    const z = ZipPack([{ name: "safe.txt", data: "x" }]);
    const bad = new Uint8Array(z);
    bad[30] = "e".charCodeAt(0);        /* the local header's name, not the CD's */
    throws(() => ZipRead(bad, "safe.txt"),
           "a local header naming a different file is refused");
}

print("test_archive: " + n + " assertions, " + fails + " failures, "
      + skips + " skipped");
if (fails)
    throw new Error("test_archive failed");
if (skips && REQUIRE_TOOLS)
    throw new Error("test_archive: skips are not allowed with tools required");
