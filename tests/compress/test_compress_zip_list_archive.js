// covers: dyna:compress.ZipList
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_zip_list_archive");
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

__kitS.section("an empty zip lists nothing", () => {
eq(ZipList(ZipPack([])).length, 0, "an empty zip lists nothing");
});

const zipBin = Which("zip");

__kitS.section("no zip(1) on this host, so reading a foreign archive is unchecked", () => {
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
});

__kitS.section("nor a zip", () => {
throws(() => ZipList(new Uint8Array([1, 2, 3])), "nor a zip");
});

__kitS.section("a zip needs an EOCD record", () => {
throws(() => ZipList(new Uint8Array(100)), "a zip needs an EOCD record");
});
__kitS.end();

print("test_archive: " + n + " assertions, " + fails + " failures, "
      + skips + " skipped");
if (fails)
    throw new Error("test_archive failed");
if (skips && REQUIRE_TOOLS)
    throw new Error("test_archive: skips are not allowed with tools required");
