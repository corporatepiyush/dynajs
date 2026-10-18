// flags: --std
// timeout: 60
import { ASN1 } from "dyna:serialize";

let n = 0, failed = 0;
function ok(c, msg) {
    n++;
    if (!c) {
        failed++;
        console.log("  FAIL " + msg);
    }
}
function hexOf(u8) {
    let s = "";
    for (const b of u8)
        s += b.toString(16).padStart(2, "0");
    return s;
}

function countTlvs(nodes) {
    const len = nodes * 2;
    let nlen = 1;
    while (nlen < 4 && (len >>> (8 * nlen)))
        nlen++;
    const blob = new Uint8Array(2 + nlen + len);
    blob[0] = 0x30;
    if (nlen === 1) {
        blob[1] = len & 0xff;
    } else {
        blob[1] = 0x80 | nlen;
        for (let i = 0; i < nlen; i++)
            blob[2 + i] = (len >>> (8 * (nlen - 1 - i))) & 0xff;
    }
    const cs = 2 + nlen;
    for (let i = 0; i < nodes; i++) {
        blob[cs + i * 2] = 0x05;
        blob[cs + 1 + i * 2] = 0x00;
    }
    return blob;
}
function manyTlvs(total) {
    const per = 60000;
    const parts = [];
    let left = total;
    while (left > 0) {
        const n = left > per ? per : left;
        parts.push(countTlvs(n));
        left -= n;
    }
    const content = parts.reduce((a, p) => a + p.length, 0);
    let nlen = 1;
    while (nlen < 4 && (content >>> (8 * nlen)))
        nlen++;
    const out = new Uint8Array(2 + nlen + content);
    out[0] = 0x30;
    if (nlen === 1) {
        out[1] = content & 0xff;
    } else {
        out[1] = 0x80 | nlen;
        for (let i = 0; i < nlen; i++)
            out[2 + i] = (content >>> (8 * (nlen - 1 - i))) & 0xff;
    }
    let off = 2 + nlen;
    for (const p of parts) {
        out.set(p, off);
        off += p.length;
    }
    return out;
}
{
    const blob = manyTlvs(1800000);
    const t0 = Date.now();
    let threw = false;
    try { ASN1.decode(blob); } catch (e) { threw = /nodes|cap|memory/i.test(String(e)); }
    const dt = Date.now() - t0;
    ok(threw, "1.2M tiny TLVs hit the decoded-node cap (threw, dt=" + dt + "ms)");
    ok(dt < 5000, "the node cap fires quickly (dt=" + dt + "ms)");
}
{
    const blob = manyTlvs(60000);
    const back = ASN1.encode(ASN1.decode(blob));
    ok(hexOf(back) === hexOf(blob), "a normal SEQUENCE of TLVs still round-trips");
}
{
    const enc = (v) => hexOf(ASN1.encode(ASN1.int(v)));
    ok(enc(-9223372036854775809n) === "0209ff7fffffffffffffff",
        "-2^63-1 encodes as 9 bytes (got " + enc(-9223372036854775809n) + ")");
    ok(enc(-18446744073709551616n) === "0209ff0000000000000000",
        "-2^64 encodes as 9 bytes (got " + enc(-18446744073709551616n) + ")");
    const wide = [
        -9223372036854775809n,
        -18446744073709551616n,
        -(2n ** 100n),
        -(2n ** 100n + 12345n),
        -123456789012345678901234567890n,
    ];
    for (const v of wide) {
        let back = null;
        try {
            const node = ASN1.decode(ASN1.encode(ASN1.int(v)));
            back = node && node.value;
        } catch (e) {
            back = "ERR " + e;
        }
        ok(String(back) === String(v), "round trip " + v + " (got " + back + ")");
    }
    ok(enc(2n ** 100n).indexOf("020d") === 0, "a wide positive still encodes (got " + enc(2n ** 100n).slice(0, 8) + ")");
}
console.log("test_asn1_bounds: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_asn1_bounds: " + failed + " failures");
