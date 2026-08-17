/* test_archive_pax.js -- the PAX extended-header surface of TarExtract/TarList.
 *
 * WHY THIS FILE EXISTS: a PAX `size=<digits>` record overrides the ustar
 * header's size (POSIX-correct), and only the header's was validated against
 * the archive. An over-declared PAX size made the reader copy heap bytes
 * past an exact-size input buffer into a JS-visible Uint8Array -- the
 * project's #1 untrusted-input surface, after its hardening pass was
 * declared done. The fix rejects `e->size > r->n - e->body` in tar_next.
 *
 * Every case here is an archive built BY HAND (the writer emits no PAX
 * records), with the ustar header recipe mirrored from the writer so the
 * failure mode is the PAX grammar, never the checksum. The load-bearing
 * rows are the over-declared sizes: they must THROW, and the message must
 * name the declaration, so a truncation-to-fit regression cannot read as
 * "works".
 */
import { TarList, TarExtract } from "dyna:compress";
import { Path, makeTempDir, writeFile, removeAll } from "dyna:file";
import { Which, Exec } from "dyna:sys";

/* dyna:file has no synchronous byte read; cat(1) through Exec is the byte
 * read, exactly as in test_archive.js. */
const readBytes = (p) => Exec("cat", [p], { encoding: "bytes" }).stdout;

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const eq = (a, b, m) => ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
const threw = (fn, re, m) => {
    try { fn(); fail++; print("  FAIL: " + m + ": did not throw"); }
    catch (e) { ok(re.test(String(e.message)), m + " -- " + String(e.message)); }
};

/* ---- the ustar header recipe (writer-mirrored) ------------------------- */
const TAR_BLOCK = 512;
function octField(buf, off, value, width) {
    const s = value.toString(8).padStart(width - 1, "0");
    for (let i = 0; i < width - 1; i++) buf[off + i] = s.charCodeAt(i);
    buf[off + width - 1] = 0;
}
function tarHeader(name, size, type = "0") {
    const h = new Uint8Array(TAR_BLOCK);
    for (let i = 0; i < name.length && i < 100; i++) h[i] = name.charCodeAt(i);
    octField(h, 100, 0o644, 8);
    octField(h, 108, 0, 8);
    octField(h, 116, 0, 8);
    octField(h, 124, size, 12);
    octField(h, 136, 0, 12);
    h[156] = type.charCodeAt(0);
    for (let i = 0; i < 5; i++) h[257 + i] = "ustar".charCodeAt(i);
    h[263] = 48; h[264] = 48;
    let sum = 0;
    for (let i = 0; i < TAR_BLOCK; i++) sum += (i >= 148 && i < 156) ? 32 : h[i];
    const cs = sum.toString(8).padStart(6, "0");
    for (let i = 0; i < 6; i++) h[148 + i] = cs.charCodeAt(i);
    h[154] = 0; h[155] = 32;
    return h;
}
function paxRecord(key, value) {
    const body = key + "=" + value + "\n";
    let len = body.length + 2;
    while (String(len).length + 1 + body.length > len) len++;
    return String(len) + " " + body;
}
const bytes = (s) => new Uint8Array([...s].map((ch) => ch.charCodeAt(0)));
/* [PAX x-header][PAX data, block-padded][real header][body]: the PAX
   grammar's on-disk shape. The reader advances by whole blocks, so the
   PAX data must occupy a multiple of 512. */
function paxArchive(records, name, size, body, type = "0") {
    const paxData = records.join("");
    const pad = (TAR_BLOCK - (paxData.length % TAR_BLOCK)) % TAR_BLOCK;
    const parts = [tarHeader("pax", paxData.length, "x"),
                   bytes(paxData + "\0".repeat(pad)),
                   tarHeader(name, size, type)];
    if (body) {
        const bpad = TAR_BLOCK - (body.length % TAR_BLOCK);
        parts.push(bytes(body + "\0".repeat(bpad)));
    }
    const out = new Uint8Array(parts.reduce((a, b) => a + b.length, 0));
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return out;
}
const dec = (u8) => new TextDecoder().decode(u8);

/* ---------------------------------------------------- THE load-bearing rows */

/* 1. The review's exact craft: PAX size=2147483647, real header small. */
{
    const a = paxArchive([paxRecord("size", "2147483647")], "f.txt", 4, "AAAA");
    threw(() => TarExtract(a), /declares 2147483647 bytes with \d+ left/,
          "TarExtract refuses a PAX size of 2147483647");
    threw(() => TarList(a), /declares 2147483647/,
          "TarList refuses it too (the check is in the reader, not the extractor)");
}

/* 2. A digit run that wraps uint64_t saturates and is refused. */
{
    const a = paxArchive([paxRecord("size", "9".repeat(30))], "f.txt", 4, "AAAA");
    threw(() => TarExtract(a), /declares/, "a 30-digit PAX size is refused");
}

/* 3. One byte over what the archive holds, with NO padding after the body:
       the exact boundary, not the padded block, is the bound. */
{
    const rec = paxRecord("size", "5");
    const pad = TAR_BLOCK - (rec.length % TAR_BLOCK);
    const hdr = tarHeader("f.txt", 4);
    const body = new Uint8Array([65, 66, 67, 68]); /* ABCD: 4 bytes */
    const a = new Uint8Array(TAR_BLOCK + rec.length + pad + TAR_BLOCK + 4);
    let o = 0;
    a.set(tarHeader("pax", rec.length, "x"), 0); o += TAR_BLOCK;
    a.set(bytes(rec), o); o += rec.length + pad;
    a.set(hdr, o); o += TAR_BLOCK;
    a.set(body, o);
    threw(() => TarExtract(a), /declares 5 bytes with 4 left/,
          "a PAX size one past the archive end names both numbers");
}

/* 4. Over-declared on a DIRECTORY entry: the bound applies to every type. */
{
    const a = paxArchive([paxRecord("size", "99999")], "d", 0, "", "5");
    threw(() => TarExtract(a), /declares 99999/,
          "a PAX size on a directory is refused too");
}

/* ------------------------------------------------- legit PAX reads (control) */

/* 5. The spec-correct happy path: a long path + an accurate size. */
{
    const long = "dir/sub/" + "x".repeat(120) + ".txt";
    const a = paxArchive([paxRecord("path", long), paxRecord("size", "3")],
                         "ignored", 3, "abc");
    const e = TarExtract(a)[0];
    eq(e.name, long, "the PAX path names the entry");
    eq(e.size, 3, "the PAX size sizes it");
    eq(dec(e.data), "abc", "and the data is whole");
}

/* 6. PAX size is AUTHORITATIVE: it wins over a larger header size. */
{
    const a = paxArchive([paxRecord("size", "3")], "f", 5, "abcde");
    const e = TarExtract(a)[0];
    eq(e.size, 3, "a PAX size of 3 beats the header's 5");
    eq(dec(e.data), "abc", "the extraction follows the PAX size");
}

/* 7. A PAX size of only non-digits parses as 0, and the header wins. */
{
    const a = paxArchive([paxRecord("size", "abc")], "f", 3, "abc");
    eq(dec(TarExtract(a)[0].data), "abc", "size=abc falls back to the header");
}

/* 8. Digits-then-junk: the digit prefix is the value. */
{
    const a = paxArchive([paxRecord("size", "12xyz")], "f", 12, "123456789012");
    eq(dec(TarExtract(a)[0].data), "123456789012", "size=12xyz reads as 12");
}

/* 9. A normal entry followed by a PAX entry: the PAX entry must not inherit
       the previous entry's size (the reader zeroes state per entry). Built
       whole by hand: TarPack's output ends in the end-of-archive zero
       blocks, which stop the reader before anything appended after it. */
{
    const recs = [paxRecord("path", "second.txt"), paxRecord("size", "6")];
    const paxData = recs.join("");
    const pad = TAR_BLOCK - (paxData.length % TAR_BLOCK);
    const parts = [
        tarHeader("first.txt", 3),
        bytes("one" + "\0".repeat(TAR_BLOCK - 3)),
        tarHeader("pax", paxData.length, "x"),
        bytes(paxData + "\0".repeat(pad)),
        tarHeader("second.txt", 6),
        bytes("second" + "\0".repeat(TAR_BLOCK - 6)),
        new Uint8Array(TAR_BLOCK * 2),           /* end of archive */
    ];
    const whole = new Uint8Array(parts.reduce((a, b) => a + b.length, 0));
    let o = 0;
    for (const p of parts) { whole.set(p, o); o += p.length; }
    const es = TarExtract(whole);
    eq(es.length, 2, "two entries read");
    eq(dec(es[0].data), "one", "the plain entry is untouched");
    eq(es[1].name, "second.txt", "the PAX entry names itself");
    eq(dec(es[1].data), "second", "and carries ITS size, not the first entry's");
}

/* 10. An unrecognised PAX key is skipped; `size` still applies. */
{
    const a = paxArchive([paxRecord("mtime", "1234"), paxRecord("size", "2")],
                         "f", 2, "hi");
    eq(dec(TarExtract(a)[0].data), "hi", "unknown PAX keys do not poison the read");
}

/* 11. A PAX block whose record lengths are garbage: the reader must not
        run away -- the header size is the fallback. */
{
    const junk = "99999999999999999999999999999999999999999999999999999999999";
    const a = paxArchive([junk], "f", 3, "abc");
    eq(dec(TarExtract(a)[0].data), "abc", "an unparsable PAX block falls back cleanly");
}

/* 12. THE FOREIGN ORACLE: an extended-name archive from the system tar.
        GNU tar uses 'L' records, bsdtar uses PAX 'x' -- the reader must
        handle both; the PAX-specific grammar is pinned by cases 5-11. */
{
    const tarBin = Which("tar");
    if (!tarBin) {
        print("  SKIP: no tar(1) on this host, the cross-implementation extended-name check cannot run");
    } else {
        const dir = String(makeTempDir("pax"));
        try {
            const long = "deep-name-" + "y".repeat(130) + ".txt";
            writeFile(new Path(dir + "/" + long), "ext-name");
            const c = Exec(tarBin, ["-cf", dir + "/out.tar", "-C", dir, long]);
            eq(c.code, 0, "the system tar writes an extended-name archive");
            const es = TarExtract(readBytes(dir + "/out.tar"));
            const mine = es.find((e) => e.name.endsWith(".txt") && !e.name.startsWith("._"));
            ok(mine !== undefined, "the long-name member is visible");
            if (mine) eq(dec(mine.data), "ext-name",
                         "and it reads byte for byte through the extended-name record");
        } finally {
            removeAll(new Path(dir));
        }
    }
}

print("test_archive_pax: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
