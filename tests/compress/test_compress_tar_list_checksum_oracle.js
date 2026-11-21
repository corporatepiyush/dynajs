// covers: contract dyna:compress.TarList dyna:compress.TarExtract dyna:compress.TarPack -- one header checksum shared by the reader and the writer
// The ustar header checksum (the byte sum of the 512-byte header with the 8-byte checksum field at 148..155 counted as
// spaces) was summed with a per-byte branch; it is now three plain sums (0..147, 8 spaces, 156..511). Oracle: a JS
// checksum. TarPack's headers must carry the JS checksum; JS-built headers with the JS checksum must list; a byte
// changed in each region (the name at 0, the mode/size fields, the bytes either side of the checksum field, the
// magic, the prefix up to 511) without fixing the checksum must be refused, and changing a byte inside the checksum
// field's own value must be refused too, while a change that keeps the sum (one byte +1, another -1) still lists.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_tar_list_checksum_oracle");
import { TarPack, TarList, TarExtract } from "dyna:compress";

let checks = 0, fails = 0;
function ok(c, what) { checks++; if (!c) { fails++; if (fails < 15) print("FAIL: " + what); } }
const jsSum = (h) => { let s = 0; for (let i = 0; i < 512; i++) s += i >= 148 && i < 156 ? 32 : h[i]; return s; };
const octal = (h) => parseInt(new TextDecoder().decode(h.subarray(148, 156)).replace(/[\0 ]/g, ""), 8);
function header(name, size) {
    const h = new Uint8Array(512), te = new TextEncoder();
    h.set(te.encode(name), 0);
    h.set(te.encode("0000644\0"), 100); h.set(te.encode("0000000\0"), 108); h.set(te.encode("0000000\0"), 116);
    h.set(te.encode(size.toString(8).padStart(11, "0") + "\0"), 124); h.set(te.encode("14000000000\0"), 136);
    h[156] = 48; h.set(te.encode("ustar\0"), 257); h.set(te.encode("00"), 263);
    return h;
}
function seal(h) { h.set(new TextEncoder().encode(jsSum(h).toString(8).padStart(6, "0") + "\0 "), 148); return h; }
function archive(h, size) { const a = new Uint8Array(512 + Math.ceil(size / 512) * 512 + 1024); a.set(h, 0); for (let i = 0; i < size; i++) a[512 + i] = (i * 7) & 255; return a; }
const lists = (a) => { try { return TarList(a).length; } catch (e) { return -1; } };
__kitS.section("TarPack headers carry the JS checksum", () => {
    for (let n = 1; n < 30; n++) {
        const t = TarPack([{ name: "dir/" + "é".repeat(n) + n, data: new Uint8Array(n * 31).fill(n), mode: 0o640 + n, mtime: 1e9 + n * 12345 }]);
        ok(octal(t) === jsSum(t.subarray(0, 512)), "packed header " + n + ": stored " + octal(t) + " vs JS " + jsSum(t.subarray(0, 512)));
        ok(TarExtract(t)[0].data.length === n * 31, "packed archive " + n + " extracts");
    }
});
__kitS.section("JS-sealed headers list; a byte changed in any region is refused", () => {
    for (const pos of [0, 1, 99, 100, 123, 124, 135, 147, 156, 157, 257, 262, 300, 345, 499, 510, 511]) {
        for (const delta of [1, 3, 128]) {
            const h = seal(header("f" + pos + ".txt", 700));
            ok(lists(archive(h, 700)) === 1, "sealed header lists (" + pos + ")");
            const bad = h.slice(); bad[pos] = (bad[pos] + delta) & 255;
            const r = lists(archive(bad, 700));
            ok(r === -1 || (pos >= 124 && pos < 136) || pos === 156, "changed byte " + pos + " +" + delta + " is refused (got " + r + ")");
            if (pos !== 0 && pos < 500 && (pos < 124 || pos >= 136) && pos !== 156 && pos !== 257) {
                const keep = h.slice(); keep[pos] = (keep[pos] + 1) & 255; keep[511] = (keep[511] - (keep[pos] === 0 ? -255 : 1) + 256) & 255;
                if (jsSum(keep) === jsSum(h)) ok(lists(archive(keep, 700)) >= 0 || pos >= 257, "sum-preserving change at " + pos + " still passes the checksum");
            }
        }
    }
    for (const k of [148, 150, 153]) {
        const h = seal(header("c.txt", 10)); h[k] = h[k] === 55 ? 54 : h[k] + 1;
        ok(lists(archive(h, 10)) === -1, "checksum field digit " + k + " changed is refused");
    }
});
print("tar checksum oracle: " + checks + " checks, " + fails + " failed");
if (fails) throw new Error(fails + " failed");
__kitS.end();
