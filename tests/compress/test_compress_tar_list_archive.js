// covers: dyna:compress.TarList
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_tar_list_archive");
import { TarPack, TarList, TarExtract, ZipPack, ZipList, ZipRead } from "dyna:compress";
import { Exec, Which, getEnv } from "dyna:sys";
import { Path, makeTempDir, writeFile, removeAll } from "dyna:file";

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
const REQUIRE_TOOLS = (getEnv("DYNAJS_REQUIRE_TOOLS") === "1");
function skip(why) {
    if (REQUIRE_TOOLS) { assert(false, "REQUIRED tool missing: " + why); return; }
    skips++; print("  SKIP: " + why);
}
const dec = (u8) => new TextDecoder().decode(u8);
const j = (v) => JSON.stringify(v);

eq(TarList(TarPack([])).length, 0, "an empty archive lists nothing");

__kitS.section("not safe", () => {
    const safe = TarPack([{ name: "aaaaaaaaaaaaa", data: "x" }]);
    const evil = new Uint8Array(safe);
    const bad = "../../etc/passwd";
    for (let i = 0; i < 13; i++) evil[i] = bad.charCodeAt(i);
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
});

__kitS.section("link target is not safe", () => {
    const t = new Uint8Array(TarPack([{ name: "l", data: "x" }]));
    t[156] = 0x32;
    const badlink = "../../x";
    for (let i = 0; i < badlink.length; i++) t[157 + i] = badlink.charCodeAt(i);
    for (let i = 148; i < 156; i++) t[i] = 32;
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
});

throws(() => TarList(new Uint8Array([1, 2, 3])), "garbage is not a tar");

throws(() => TarList(), "an archive is required");

__kitS.section("25 mtime=1700000000", () => {
    const mkhdr = ({ name = "", size = 0, type = "0", mode = 0o644,
                     mtime = 0, linkname = "", prefix = "" }) => {
        const h = new Uint8Array(512);
        const put = (off, str, max) => {
            for (let i = 0; i < Math.min(str.length, max); i++)
                h[off + i] = str.charCodeAt(i);
        };
        const oct = (off, v, n) => {
            let t = Math.floor(v).toString(8);
            if (t.length > n - 1) t = "0".repeat(n - 1);
            put(off, t.padStart(n - 1, "0") + "\0", n);
        };
        put(0, name, 100); oct(100, mode, 8); oct(108, 0, 8); oct(116, 0, 8);
        oct(124, size, 12); oct(136, mtime, 12); h[156] = type.charCodeAt(0);
        put(157, linkname, 100); put(257, "ustar\0", 6); put(263, "00", 2);
        put(345, prefix, 155);
        let sum = 0;
        for (let i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? 32 : h[i];
        put(148, sum.toString(8).padStart(6, "0") + "\0 ", 8);
        return h;
    };
    const cat = (...as) => {
        let len = 0; for (const a of as) len += a.length;
        const o = new Uint8Array(len); let p = 0;
        for (const a of as) { o.set(a, p); p += a.length; }
        return o;
    };
    const pad = (k) => new Uint8Array(((k + 511) & ~511) - k || 0);
    const body = (s) => { const b = new TextEncoder().encode(s); return cat(b, pad(b.length)); };
    const end = () => new Uint8Array(1024);

    {
        const gbody = body("25 mtime=1700000000\n");
        const t = cat(mkhdr({ name: "gbl", size: gbody.length - 0, type: "g" }),
                      gbody, mkhdr({ name: "real.txt" }), end());
        const ls = TarList(t);
        eq(ls.length, 1, "a PAX global header is skipped, not listed");
        eq(ls[0].name, "real.txt", "the entry after the global header parses");
    }
    {
        const target = "somewhere/deeper/target.txt";
        const kbody = body(target);
        const t = cat(mkhdr({ name: "lnk", size: kbody.length, type: "K" }),
                      kbody,
                      mkhdr({ name: "file.txt", type: "2", linkname: "short" }),
                      end());
        const ls = TarList(t);
        eq(ls.length, 1, "a GNU long-link record is not listed as a file");
        eq(ls[0].name, "file.txt", "the linked entry parses");
        eq(ls[0].linkname, target, "the long link target overrides the header field");
    }
    {
        const rec = "12 size=600\n";
        const pax = body(rec);
        const data = cat(new TextEncoder().encode("d".repeat(600)), pad(600));
        const next = mkhdr({ name: "after.txt" });
        const t = cat(mkhdr({ name: "meta", size: rec.length, type: "x" }), pax,
                      mkhdr({ name: "big", size: 1 }), data,
                      next, end());
        const ls = TarList(t);
        eq(ls.length, 2, "a PAX-size member is one entry plus the next");
        eq(ls[0].size, 600, "the PAX size overrides the header's size field");
        eq(ls[1].name, "after.txt", "the next header starts after the PAX-size member");
    }
    throws(() => TarPack([{ name: "a", mtime: 2 ** 40 }]),
           "an mtime past ustar's 11-octal-digit field is refused");
    eq(TarList(TarPack([{ name: "a", mtime: 0o77777777777 }]))[0].mtime,
       0o77777777777, "ustar's maximum mtime round-trips");
    eq(TarList(TarPack([{ name: "d/", type: "directory" }]))[0].type,
       "directory", "type \"directory\" is honored");
    {
        const ls = TarList(TarPack([{ name: "z", type: "file" }]));
        eq(ls[0].type, "file", "type \"file\" wins over the no-data heuristic");
        eq(ls[0].size, 0, "an explicit file with no data is a zero-byte file");
    }
    throws(() => TarPack([{ name: "s", type: "symlink", data: "x" }]),
           "a link type is refused: this writer carries no link metadata");
    {
        const t = TarPack([{ name: "d/", type: "directory", data: "stray" },
                           { name: "after.txt", data: "ok" }]);
        const ls = TarList(t);
        eq(ls.length, 2, "directory-with-data packs a consistent archive");
        eq(ls[0].type, "directory", "the entry stays a directory");
        eq(ls[0].size, 0, "its size field is 0");
        eq(ls[1].name, "after.txt", "the next header parses (no desync)");
        eq(dec(TarExtract(t)[1].data), "ok", "the following member round-trips");
    }
});
__kitS.end();

print("test_archive: " + n + " assertions, " + fails + " failures, "
      + skips + " skipped");
if (fails)
    throw new Error("test_archive failed");
if (skips && REQUIRE_TOOLS)
    throw new Error("test_archive: skips are not allowed with tools required");
