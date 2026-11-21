// covers: dyna:compress.ZipPack
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_zip_pack_archive");
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

throws(() => ZipPack([{ data: "x" }]), "every entry needs a name");

throws(() => ZipPack("not an array"), "entries must be an array");

throws(() => ZipPack([{ name: "a", data: "x" }], { method: "lzma" }),
       "an unsupported method is refused");

__kitS.section("the quick brown fox", () => {
    const z = ZipPack([{ name: "a.txt", data: "the quick brown fox " .repeat(10) }]);
    const bad = new Uint8Array(z);
    let cd = -1;
    for (let i = bad.length - 22; i >= 0; i--)
        if (bad[i] === 0x50 && bad[i + 1] === 0x4b && bad[i + 2] === 0x01
            && bad[i + 3] === 0x02) { cd = i; break; }
    assert(cd > 0, "the central directory is where the format says");
    bad[cd + 16] ^= 0xff;
    let msg = "";
    try { ZipRead(bad, "a.txt"); } catch (e) { msg = String(e.message); }
    assert(msg.length > 0, "a corrupted CRC is refused (" + msg + ")");
});

__kitS.section("a local header naming a different file is refused", () => {
    const z = ZipPack([{ name: "safe.txt", data: "x" }]);
    const bad = new Uint8Array(z);
    bad[30] = "e".charCodeAt(0);
    throws(() => ZipRead(bad, "safe.txt"),
           "a local header naming a different file is refused");
});
__kitS.end();

print("test_archive: " + n + " assertions, " + fails + " failures, "
      + skips + " skipped");
if (fails)
    throw new Error("test_archive failed");
if (skips && REQUIRE_TOOLS)
    throw new Error("test_archive: skips are not allowed with tools required");
