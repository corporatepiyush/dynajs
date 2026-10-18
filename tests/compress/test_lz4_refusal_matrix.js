// flags: --std
import { lz4Frame, lz4Unframe } from "dyna:compress";

let fails = 0;
function ok(c, w) { if (c) print("  ok    " + w); else { fails++; print("  FAIL  " + w); } }
const enc = (s) => new TextEncoder().encode(s);
const le32 = (v) => [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255];

function frame(o) {
    const out = [];
    out.push(...le32(0x184D2204));
    out.push(o.flg !== undefined ? o.flg : 0x60, o.bd !== undefined ? o.bd : 0x40);
    const flg = o.flg !== undefined ? o.flg : 0x60;
    if (flg & 0x08) out.push(...le32((o.contentSize >>> 0) || 0), 0, 0, 0, 0);
    if (flg & 0x01) out.push(...le32(o.dictId || 0));
    out.push(o.hc !== undefined ? o.hc : 0);
    for (const b of (o.blocks || [])) {
        const bytes = b.stored || b.raw;
        if (b.stored) out.push(...le32(0x80000000 | bytes.length));
        else out.push(...le32(bytes.length));
        for (const x of bytes) out.push(x);
        if (flg & 0x10) out.push(...le32(o.blockSum !== undefined ? o.blockSum : 0));
    }
    out.push(0, 0, 0, 0);
    if (flg & 0x04) out.push(...le32(o.contentSum !== undefined ? o.contentSum : 0));
    return Uint8Array.from(out);
}

const PREFIX = "dyna:compress lz4Unframe: ";
function refusal(u) {
    try { lz4Unframe(u); return null; } catch (e) { return String(e.message); }
}

function findHc(buildWithHc) {
    const accepted = [];
    for (let hc = 0; hc < 256; hc++) {
        const msg = refusal(buildWithHc(hc));
        if (msg === null || msg.indexOf("descriptor checksum mismatch") < 0)
            accepted.push({ hc, msg });
    }
    return accepted;
}

const payload = enc("probe payload abcabcabcabcabcabcabc");
ok(lz4Unframe(lz4Frame(payload)).length === payload.length,
   "valid control decodes");

const classSeen = new Map();
function want(label, suffix, msg) {
    ok(msg === PREFIX + suffix,
       label + " -> " + (msg === null ? "DECODED (want refusal)" : msg));
    if (msg !== null) {
        const prev = classSeen.get(msg);
        if (prev && prev !== suffix)
            ok(false, "CLASS DRIFT: [" + msg + "] claimed by " + prev + " and " + suffix);
        if (!classSeen.has(msg)) classSeen.set(msg, suffix);
    }
}

want("trunc<4", "truncated LZ4 frame",
     refusal(new Uint8Array([1, 2, 3])));
want("magic", "not an LZ4 frame (bad magic)",
     refusal(new Uint8Array([1, 2, 3, 4, 5])));
want("trunc hdr", "truncated LZ4 frame",
     refusal(new Uint8Array([0x04, 0x22, 0x4d, 0x18, 0x60])));

{
    const acc = findHc(hc => frame({ flg: 0x00, hc, blocks: [{ stored: payload }] }));
    ok(acc.length === 256, "VERSION shape: refused pre-HC, HC-insensitive (got " + acc.length + ")");
    want("version", "unsupported LZ4 frame version",
         acc.length ? acc[0].msg : "NONE");
}
{
    const acc = findHc(hc => frame({ flg: 0x40, hc, blocks: [{ stored: payload }] }));
    ok(acc.length === 256, "LINKED shape: refused pre-HC, HC-insensitive (got " + acc.length + ")");
    want("linked", "linked LZ4 blocks are not supported",
         acc.length ? acc[0].msg : "NONE");
}
{
    const acc = findHc(hc => frame({ flg: 0x20, hc, blocks: [{ stored: payload }] }));
    ok(acc.length === 256, "version+linked shape: refused pre-HC, HC-insensitive");
    want("version+linked (order)", "unsupported LZ4 frame version",
         acc.length ? acc[0].msg : "NONE");
}
{
    const acc = findHc(hc => frame({ flg: 0x60, bd: 0x30, hc, blocks: [{ stored: payload }] }));
    ok(acc.length === 256, "BLOCKMAX shape: refused pre-HC, HC-insensitive");
    want("blockmax", "invalid LZ4 block-max size", acc.length ? acc[0].msg : "NONE");
}
{
    const acc = findHc(hc => frame({ flg: 0x60, hc, blocks: [{ stored: payload }] }));
    ok(acc.length === 1, "healthy shape: exactly 1 accepted HC byte (got " + acc.length + ")");
    want("hdrsum", "LZ4 frame descriptor checksum mismatch",
         refusal(frame({ flg: 0x60, hc: 0x5a, blocks: [{ stored: payload }] })));
}

function wantHc(label, suffix, shape) {
    const acc = findHc(hc => shape(hc));
    ok(acc.length === 1, label + " shape: exactly 1 accepted HC byte (got " + acc.length + ")");
    want(label, suffix, acc.length ? acc[0].msg : "NONE");
}

{
    const big = new Uint8Array(70000);
    wantHc("blockcap", "LZ4 block exceeds the declared block-max size",
           hc => frame({ flg: 0x60, hc, blocks: [{ stored: big }] }));
    wantHc("blockcap over decode (order)",
           "LZ4 block exceeds the declared block-max size",
           hc => frame({ flg: 0x60, hc, blocks: [{ raw: big }] }));
}
{
    wantHc("block", "corrupt LZ4 block",
           hc => frame({ flg: 0x60, hc, blocks: [{ raw: [0xff, 0xff] }] }));
    wantHc("block over blksum (order)", "corrupt LZ4 block",
           hc => frame({ flg: 0x70, hc, blocks: [{ raw: [0xff, 0xff] }], blockSum: 0 }));
    wantHc("blksum", "LZ4 block checksum mismatch",
           hc => frame({ flg: 0x70, hc, blocks: [{ stored: payload }], blockSum: 0xdeadbeef }));
}
{
    wantHc("contsum", "LZ4 content checksum mismatch",
           hc => frame({ flg: 0x64, hc, blocks: [{ stored: payload }], contentSum: 0xdeadbeef }));
    wantHc("contsize", "LZ4 frame declared content size mismatch",
           hc => frame({ flg: 0x68, hc, contentSize: payload.length + 1,
                        blocks: [{ stored: payload }] }));
}
{
    wantHc("trunc block", "truncated LZ4 frame", hc => {
        const f = frame({ flg: 0x60, hc, blocks: [] });
        const g = new Uint8Array(f.length - 4 + 4 + 3);
        g.set(f.subarray(0, f.length - 4));
        g[f.length - 4] = 8; g[f.length - 3] = 0;
        g[f.length - 2] = 0; g[f.length - 1] = 0;
        return g;
    });
}

const msgs = [...classSeen.keys()];
ok(new Set(msgs).size === msgs.length, "observed messages pairwise distinct");
ok(msgs.length >= 11, ">= 11 distinct refusal classes observed (got " + msgs.length + ")");
print("probe D: " + msgs.length + " classes, fails=" + fails);
if (fails) throw new Error("probe D fails");
