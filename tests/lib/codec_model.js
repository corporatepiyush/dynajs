// Reference byte codecs written from their specs in plain JS: an oracle for dyna:encoding,
// dyna:url punycode and the gzip container that shares no code with the engine.
// utf8(str) -> Uint8Array (lone surrogates become U+FFFD, as TextEncoder does);
// ENCODERS[name](bytes) -> string for HexEncode, Base64Encode, Base64URLEncode (RFC 4648,
// the URL form unpadded), Base32Encode, Base32HexEncode (padded), Base58Encode (bitcoin
// alphabet, leading zero bytes as '1'), Base85Encode (ascii85, no <~ ~>, 'z' for a zero group
// except the final partial group); punycode(str) -> RFC 3492 encoding of the code points;
// crc32(bytes) -> the IEEE CRC-32 a gzip trailer carries.
export function utf8(s) {
    const out = [];
    for (let i = 0; i < s.length; i++) {
        let c = s.charCodeAt(i);
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
            const d = s.charCodeAt(i + 1);
            if (d >= 0xdc00 && d <= 0xdfff) { c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00); i++; }
            else c = 0xfffd;
        } else if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;
        if (c < 0x80) out.push(c);
        else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
        else if (c < 0x10000) out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
        else out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
    }
    return new Uint8Array(out);
}
export const bytesOf = (x) => (typeof x === "string" ? utf8(x) : x);
export const sameBytes = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);

function radix2(b, bits, alpha, pad) {
    let out = "", acc = 0, n = 0;
    for (const v of b) {
        acc = (acc << 8) | v; n += 8;
        while (n >= bits) { n -= bits; out += alpha[(acc >> n) & ((1 << bits) - 1)]; }
        acc &= (1 << n) - 1;
    }
    if (n > 0) out += alpha[(acc << (bits - n)) & ((1 << bits) - 1)];
    if (pad) while (out.length % pad) out += "=";
    return out;
}
const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const B64U = B64.slice(0, 62) + "-_";
const B32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
const B32H = "0123456789ABCDEFGHIJKLMNOPQRSTUV";
const B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

function base58(b) {
    let zeros = 0;
    while (zeros < b.length && b[zeros] === 0) zeros++;
    let n = 0n;
    for (const v of b) n = n * 256n + BigInt(v);
    let s = "";
    while (n > 0n) { s = B58[Number(n % 58n)] + s; n /= 58n; }
    return "1".repeat(zeros) + s;
}
function ascii85(b) {
    let out = "";
    for (let i = 0; i < b.length; i += 4) {
        const k = Math.min(4, b.length - i);
        let v = 0;
        for (let j = 0; j < 4; j++) v = v * 256 + (j < k ? b[i + j] : 0);
        if (k === 4 && v === 0) { out += "z"; continue; }
        const d = [];
        for (let j = 0; j < 5; j++) { d.unshift(String.fromCharCode(33 + (v % 85))); v = Math.floor(v / 85); }
        out += d.join("").slice(0, k + 1);
    }
    return out;
}
export const ENCODERS = {
    HexEncode: (b) => Array.from(b, (v) => v.toString(16).padStart(2, "0")).join(""),
    Base64Encode: (b) => radix2(b, 6, B64, 4),
    Base64URLEncode: (b) => radix2(b, 6, B64U, 0),
    Base32Encode: (b) => radix2(b, 5, B32, 8),
    Base32HexEncode: (b) => radix2(b, 5, B32H, 8),
    Base58Encode: base58,
    Base85Encode: ascii85,
};

export function punycode(s) {
    const cps = Array.from(s, (c) => c.codePointAt(0));
    const base = 36, tmin = 1, tmax = 26, skew = 38, damp = 700;
    const digit = (d) => String.fromCharCode(d < 26 ? 97 + d : 22 + d);
    const adapt = (delta, num, first) => {
        delta = first ? Math.floor(delta / damp) : delta >> 1;
        delta += Math.floor(delta / num);
        let k = 0;
        while (delta > ((base - tmin) * tmax) >> 1) { delta = Math.floor(delta / (base - tmin)); k += base; }
        return k + Math.floor(((base - tmin + 1) * delta) / (delta + skew));
    };
    let out = cps.filter((c) => c < 0x80).map((c) => String.fromCharCode(c)).join("");
    const b = out.length;
    let h = b, n = 128, delta = 0, bias = 72;
    if (b > 0) out += "-";
    while (h < cps.length) {
        const m = Math.min(...cps.filter((c) => c >= n));
        delta += (m - n) * (h + 1);
        n = m;
        for (const c of cps) {
            if (c < n) delta++;
            if (c === n) {
                let q = delta;
                for (let k = base; ; k += base) {
                    const t = k <= bias ? tmin : k >= bias + tmax ? tmax : k - bias;
                    if (q < t) break;
                    out += digit(t + ((q - t) % (base - t)));
                    q = Math.floor((q - t) / (base - t));
                }
                out += digit(q);
                bias = adapt(delta, h + 1, h === b);
                delta = 0; h++;
            }
        }
        delta++; n++;
    }
    return out;
}

const CRC = (() => {
    const t = new Uint32Array(256);
    for (let i = 0; i < 256; i++) { let c = i; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[i] = c >>> 0; }
    return t;
})();
export function crc32(b) {
    let c = 0xffffffff;
    for (const v of b) c = CRC[(c ^ v) & 255] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
}
