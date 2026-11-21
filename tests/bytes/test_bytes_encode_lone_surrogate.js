// covers: dyna:bytes.encode
// P15 policy: every string -> UTF-8 byte sink writes well-formed UTF-8, a lone surrogate as
// U+FFFD (EF BF BD) exactly like TextEncoder: bytes.encode(.., "utf-8"), file.writeFile (plain and
// atomic), FileWriter.write and
// MsgPack/CBOR strings. A real surrogate pair stays one 4-byte sequence.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_encode_lone_surrogate");
import * as bytes from "dyna:bytes";
import * as file from "dyna:file";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
function same(a, b, msg) { assert(a === b, msg + " (got " + a + ", want " + b + ")"); }
const cases = [
    ["\ud800", "efbfbd"], ["a\udc00b", "61efbfbd62"], ["\udc00\ud800", "efbfbdefbfbd"],
    ["😀", "f09f9880"], ["x\ud83d", "78efbfbd"], ["\u0000", "00"],
];

__kitS.section("bytes.encode utf-8", () => {
    for (const [s, h] of cases) {
        same(hex(bytes.encode(s, "utf-8")), h, "encode " + escape(s));
        same(hex(bytes.encode(s, "utf8")), hex(new TextEncoder().encode(s)), "matches TextEncoder " + escape(s));
    }
});

__kitS.section("file.writeFile string", () => {
    const d = file.makeTempDir("p15enc");
    try {
        const p = d.join("f.txt");
        for (const [s, h] of cases) {
            const w = file.writeFile(p, s);
            same(hex(file.readBytes(p)), h, "writeFile " + escape(s));
            same(w, h.length / 2, "byte count " + escape(s));
        }
        const fw = new file.File(p).writer();
        for (const [s] of cases) fw.write(s);
        fw.close();
        same(hex(file.readBytes(p)), cases.map(([, h]) => h).join(""), "FileWriter.write strings");
        file.writeFile(p, "\ud800", { atomic: true });
        same(hex(file.readBytes(p)), "efbfbd", "atomic writeFile");
        file.writeFile(p, { toString() { return "x\udc00"; } });
        same(hex(file.readBytes(p)), "78efbfbd", "a coerced non-string payload");
    } finally {
        file.removeAll(d);
    }
});

print("test_bytes_encode_lone_surrogate: " + n + " assertions, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
