import { TarList, TarExtract } from "dyna:compress";
import { Path, makeTempDir, writeFile, removeAll } from "dyna:file";
import { Which, Exec } from "dyna:sys";

const readBytes = (p) => Exec("cat", [p], { encoding: "bytes" }).stdout;

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const eq = (a, b, m) => ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
const threw = (fn, re, m) => {
    try { fn(); fail++; print("  FAIL: " + m + ": did not throw"); }
    catch (e) { ok(re.test(String(e.message)), m + " -- " + String(e.message)); }
};

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

{
    const a = paxArchive([paxRecord("size", "2147483647")], "f.txt", 4, "AAAA");
    threw(() => TarExtract(a), /declares 2147483647 bytes with \d+ left/,
          "TarExtract refuses a PAX size of 2147483647");
    threw(() => TarList(a), /declares 2147483647/,
          "TarList refuses it too (the check is in the reader, not the extractor)");
}

{
    const a = paxArchive([paxRecord("size", "9".repeat(30))], "f.txt", 4, "AAAA");
    threw(() => TarExtract(a), /declares/, "a 30-digit PAX size is refused");
}

{
    const rec = paxRecord("size", "5");
    const pad = TAR_BLOCK - (rec.length % TAR_BLOCK);
    const hdr = tarHeader("f.txt", 4);
    const body = new Uint8Array([65, 66, 67, 68]);
    const a = new Uint8Array(TAR_BLOCK + rec.length + pad + TAR_BLOCK + 4);
    let o = 0;
    a.set(tarHeader("pax", rec.length, "x"), 0); o += TAR_BLOCK;
    a.set(bytes(rec), o); o += rec.length + pad;
    a.set(hdr, o); o += TAR_BLOCK;
    a.set(body, o);
    threw(() => TarExtract(a), /declares 5 bytes with 4 left/,
          "a PAX size one past the archive end names both numbers");
}

{
    const a = paxArchive([paxRecord("size", "99999")], "d", 0, "", "5");
    threw(() => TarExtract(a), /declares 99999/,
          "a PAX size on a directory is refused too");
}

{
    const long = "dir/sub/" + "x".repeat(120) + ".txt";
    const a = paxArchive([paxRecord("path", long), paxRecord("size", "3")],
                         "ignored", 3, "abc");
    const e = TarExtract(a)[0];
    eq(e.name, long, "the PAX path names the entry");
    eq(e.size, 3, "the PAX size sizes it");
    eq(dec(e.data), "abc", "and the data is whole");
}

{
    const a = paxArchive([paxRecord("size", "3")], "f", 5, "abcde");
    const e = TarExtract(a)[0];
    eq(e.size, 3, "a PAX size of 3 beats the header's 5");
    eq(dec(e.data), "abc", "the extraction follows the PAX size");
}

{
    const a = paxArchive([paxRecord("size", "abc")], "f", 3, "abc");
    eq(dec(TarExtract(a)[0].data), "abc", "size=abc falls back to the header");
}

{
    const a = paxArchive([paxRecord("size", "12xyz")], "f", 12, "123456789012");
    eq(dec(TarExtract(a)[0].data), "123456789012", "size=12xyz reads as 12");
}

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
        new Uint8Array(TAR_BLOCK * 2),
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

{
    const a = paxArchive([paxRecord("mtime", "1234"), paxRecord("size", "2")],
                         "f", 2, "hi");
    eq(dec(TarExtract(a)[0].data), "hi", "unknown PAX keys do not poison the read");
}

{
    const junk = "99999999999999999999999999999999999999999999999999999999999";
    const a = paxArchive([junk], "f", 3, "abc");
    eq(dec(TarExtract(a)[0].data), "abc", "an unparsable PAX block falls back cleanly");
}

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
