// tests/lib/svg_png.js -- a minimal PNG decoder for the dyna:svg suites: 8-bit RGBA, non-interlaced, every filter type,
// and every chunk CRC checked. decodePNG(bytes) -> { width, height, data: Uint8Array RGBA }; it throws on anything else.
import { inflate } from "dyna:compress";
import { CRC32 } from "dyna:crypto";

const SIG = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];

function be32(b, o) {
    return ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;
}

function paeth(a, b, c) {
    const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

export function decodePNG(b) {
    for (let i = 0; i < 8; i++) if (b[i] !== SIG[i]) throw new Error("not a PNG signature");
    let o = 8, width = 0, height = 0, ended = false;
    const idat = [];
    while (o < b.length) {
        const len = be32(b, o), type = String.fromCharCode(b[o + 4], b[o + 5], b[o + 6], b[o + 7]);
        const body = b.subarray(o + 8, o + 8 + len);
        if (CRC32(b.subarray(o + 4, o + 8 + len)) >>> 0 !== be32(b, o + 8 + len)) throw new Error("bad CRC in " + type);
        if (type === "IHDR") {
            width = be32(body, 0);
            height = be32(body, 4);
            if (body[8] !== 8 || body[9] !== 6 || body[12] !== 0) throw new Error("only 8-bit RGBA non-interlaced");
        } else if (type === "IDAT") idat.push(body);
        else if (type === "IEND") { ended = true; break; }
        o += 12 + len;
    }
    if (!ended) throw new Error("no IEND");
    let n = 0;
    for (const c of idat) n += c.length;
    const z = new Uint8Array(n);
    n = 0;
    for (const c of idat) { z.set(c, n); n += c.length; }
    const raw = inflate(z, { format: "zlib" });
    const stride = width * 4, data = new Uint8Array(stride * height);
    if (raw.length !== (stride + 1) * height) throw new Error("bad image data length " + raw.length);
    for (let y = 0; y < height; y++) {
        const f = raw[y * (stride + 1)], src = y * (stride + 1) + 1, dst = y * stride;
        for (let x = 0; x < stride; x++) {
            const a = x >= 4 ? data[dst + x - 4] : 0, up = y ? data[dst - stride + x] : 0;
            const c = x >= 4 && y ? data[dst - stride + x - 4] : 0;
            let v = raw[src + x];
            if (f === 1) v += a;
            else if (f === 2) v += up;
            else if (f === 3) v += (a + up) >> 1;
            else if (f === 4) v += paeth(a, up, c);
            else if (f !== 0) throw new Error("bad filter " + f);
            data[dst + x] = v & 255;
        }
    }
    return { width, height, data };
}
