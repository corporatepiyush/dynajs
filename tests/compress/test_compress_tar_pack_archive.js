// covers: dyna:compress.TarPack
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_tar_pack_archive");
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

__kitS.section("a tar archive is a whole number of 512-byte blocks", () => {
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
});

__kitS.section("a 125-byte name survives through the prefix field", () => {
    const deep = "a/".repeat(60) + "f.txt";
    eq(TarList(TarPack([{ name: deep, data: "x" }]))[0].name, deep,
       "a 125-byte name survives through the prefix field");
    throws(() => TarPack([{ name: "z".repeat(120), data: "x" }]),
           "an unsplittable long name is refused, not truncated");
});

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

        writeFile(new Path(dir + "/src.txt"), "made by tar\n");
        const c = Exec(tarBin, ["-cf", dir + "/in.tar", "-C", dir, "src.txt"]);
        eq(c.code, 0, "the system tar writes an archive");
        const entries = TarExtract(readBytes(dir + "/in.tar"));
        const f = entries.filter((e) => e.name === "src.txt")[0];
        assert(f !== undefined, "which this module finds (" +
               entries.map((e) => e.name).join(",") + ")");
        if (f) eq(dec(f.data), "made by tar\n", "and reads byte for byte");
    } finally {
        removeAll(new Path(dir));
    }
}

__kitS.section("the tar writer refuses every unsafe name", () => {
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
});
__kitS.end();

throws(() => TarPack([{ name: "a", mode: -1 }]), "a negative mode");

print("test_archive: " + n + " assertions, " + fails + " failures, "
      + skips + " skipped");
if (fails)
    throw new Error("test_archive failed");
if (skips && REQUIRE_TOOLS)
    throw new Error("test_archive: skips are not allowed with tools required");
