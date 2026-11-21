// flags: --std
// tools/js/gen-crc.js -- regenerate the slicing-by-8 CRC32 tables, IEEE (0xEDB88320) and
// Castagnoli (0x82F63B78), that live in src/core/dyn-hash.c (crc32_ieee_s8 / crc32c_s8). It emits
// exactly the committed form (comment-free, clang-formatted), so tools/js/check-generated.js can
// regenerate and compare; it self-checks four entries against the published values (0x77073096,
// 0xF26B8303, 0x191B3141, 0x493C7D27) and refuses to emit on a mismatch.
//
// Usage: ./dynajs --std tools/js/gen-crc.js [out]   (no argument: stdout)
import * as std from "std";
import { writeText } from "./pyshim.js";

const IEEE = 0xedb88320, CAST = 0x82f63b78;
function tables(poly) {
    const t = [];
    for (let k = 0; k < 8; k++) t.push(new Array(256).fill(0));
    for (let n = 0; n < 256; n++) {
        let c = n;
        for (let k = 0; k < 8; k++) c = ((c >>> 1) ^ (poly & -(c & 1))) >>> 0;
        t[0][n] = c;
    }
    for (let n = 0; n < 256; n++) {
        let c = t[0][n];
        for (let k = 1; k < 8; k++) {
            c = (t[0][c & 0xff] ^ (c >>> 8)) >>> 0;
            t[k][n] = c;
        }
    }
    return t;
}
const hex8 = (v) => "0x" + v.toString(16).toUpperCase().padStart(8, "0");

function emit(name, t, out) {
    out.push(`static const uint32_t ${name}[8][256] = {`);
    for (const row of t) {
        out.push("    {");
        for (const v of row) out.push("        " + hex8(v) + "u,");
        out.push("    },");
    }
    out.push("};");
}

export function generate() {
    const ieee = tables(IEEE), cast = tables(CAST);
    const known = [[ieee[0][1], 0x77073096], [cast[0][1], 0xf26b8303], [ieee[1][1], 0x191b3141], [cast[7][1], 0x493c7d27]];
    for (const [got, want] of known)
        if (got !== want) throw new Error(`gen-crc: self-check failed: ${hex8(got)} != published ${hex8(want)}`);
    const out = [];
    emit("crc32_ieee_s8", ieee, out);
    out.push("");
    emit("crc32c_s8", cast, out);
    return out.join("\n") + "\n";
}

if (/gen-crc\.js$/.test(scriptArgs[0] || "")) {
    const text = generate();
    if (scriptArgs.length > 1) writeText(scriptArgs[1], text);
    else std.out.puts(text);
}
