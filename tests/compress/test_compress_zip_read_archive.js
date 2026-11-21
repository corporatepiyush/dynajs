// covers: dyna:compress.ZipRead
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_zip_read_archive");
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

__kitS.section("hello world", () => {
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
});

eq(dec(ZipRead(ZipPack([{ name: "e", data: "" }], { method: "store" }), "e")), "",
   "an empty member");

__kitS.section("a 200 KB member", () => {
    const big = "z".repeat(200000);
    eq(dec(ZipRead(ZipPack([{ name: "big", data: big }]), "big")), big,
       "a 200 KB member");
});
__kitS.end();

throws(() => ZipRead(ZipPack([{ name: "a", data: "x" }])), "ZipRead needs a name");

print("test_archive: " + n + " assertions, " + fails + " failures, "
      + skips + " skipped");
if (fails)
    throw new Error("test_archive failed");
if (skips && REQUIRE_TOOLS)
    throw new Error("test_archive: skips are not allowed with tools required");
