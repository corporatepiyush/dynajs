// flags: --std
import { Base58Encode, Base58CheckEncode, base58DecodeInto,
         base58CheckDecodeInto, BaseXEncode } from "dyna:encoding";
import * as std from "std";

const MAX = 4096;
const HEX = "0123456789abcdef";

const LENS = [];
for (let len = 0; len <= 512; len++) LENS.push(len);
for (let len = 549; len <= MAX; len += 37) LENS.push(len);
for (const b of [1024, 2048, 3072, 4095, 4096])
    if (LENS.indexOf(b) < 0) LENS.push(b);
LENS.sort((a, b) => a - b);
let nNonempty = 0;
for (const len of LENS) if (len > 0) nNonempty++;

const big = new Uint8Array(MAX + 1);
const bigZeros = new Uint8Array(MAX + 1);
const bigFfs = new Uint8Array(MAX + 1).fill(0xff);
const bigAlt = new Uint8Array(MAX + 1);
for (let i = 0; i <= MAX; i++) { big[i] = (i * 37 + 11) & 0xff; bigAlt[i] = (i & 1) ? 0xff : 0x01; }
const out = new Uint8Array(MAX + 64);

const textsOnes = [], textsZs = [], textsMixed = [], textsSliced = [];
const srcSliced = "z".repeat(MAX);
const viewsZeros = [], viewsFfs = [], viewsAlt = [], viewsMixed = [];
for (const len of LENS) {
    textsOnes.push("1".repeat(len));
    textsZs.push("z".repeat(len));
    textsMixed.push("S".padEnd(len, "tV1DL6CwTryKy"));
    textsSliced.push(srcSliced.slice(0, len));
    viewsZeros.push(bigZeros.subarray(0, len));
    viewsFfs.push(bigFfs.subarray(0, len));
    viewsAlt.push(bigAlt.subarray(0, len));
    viewsMixed.push(big.subarray(0, len));
}
const checkTexts = [];
for (let len = 0; len <= 64; len++)
    checkTexts.push(Base58CheckEncode(big.slice(0, len)));

const lensZeros = [], lensFfs = [], lensAlt = [], lensMixed = [],
      lensCheck = [], lensBasex = [];
for (let i = 0; i < LENS.length; i++) {
    lensZeros.push(Base58Encode(viewsZeros[i]).length);
    lensFfs.push(Base58Encode(viewsFfs[i]).length);
    lensAlt.push(Base58Encode(viewsAlt[i]).length);
    lensMixed.push(Base58Encode(viewsMixed[i]).length);
    lensCheck.push(Base58CheckEncode(viewsMixed[i]).length);
    lensBasex.push(BaseXEncode(viewsMixed[i], HEX).length);
}
const calUnit = "x";
const calTwo = "xy";

const probeView = bigZeros.subarray(0, 8);

function region(mark, fn) {
    std.getenv(mark);
    print(mark);
    std.out.flush();
    fn();
    std.getenv(MEND);
    print(MEND);
    std.out.flush();
}
const MEND = "##ALLOC-END##";
const M = {
    warm: "##ALLOC-BEGIN warmup##",
    base: "##ALLOC-BEGIN baseline##",
    ones: "##ALLOC-BEGIN decode-into-ones##",
    zs: "##ALLOC-BEGIN decode-into-zs##",
    mixed: "##ALLOC-BEGIN decode-into-mixed##",
    chk: "##ALLOC-BEGIN check-decode-into##",
    ez: "##ALLOC-BEGIN encode-zeros##",
    ef: "##ALLOC-BEGIN encode-ffs##",
    ea: "##ALLOC-BEGIN encode-alt##",
    em: "##ALLOC-BEGIN encode-mixed##",
    ce: "##ALLOC-BEGIN check-encode##",
    bx: "##ALLOC-BEGIN basex-encode##",
    kz: "##ALLOC-BEGIN cal-zeros##",
    kf: "##ALLOC-BEGIN cal-ffs##",
    ka: "##ALLOC-BEGIN cal-alt##",
    km: "##ALLOC-BEGIN cal-mixed##",
    kc: "##ALLOC-BEGIN cal-check##",
    kx: "##ALLOC-BEGIN cal-basex##",
    pp: "##ALLOC-BEGIN pool-probe##",
};

print("##SAMPLE lens=" + LENS.length + " nonempty=" + nNonempty +
      " checkcalls=" + checkTexts.length + " probe=32##");
std.out.flush();

region(M.warm, () => {
    let x = 0;
    for (let i = 0; i <= 8; i++) x += i;
    if (x < 0) print("never");
});

region(M.base, () => {
    let x = 0;
    for (let i = 0; i < LENS.length; i++) x += LENS[i];
    for (let i = 0; i <= 64; i++) x += i;
    for (let k = 0; k < 3; k++) x += k;
    if (x < 0) print("never");
});

region(M.ones, () => {
    for (let i = 0; i < LENS.length; i++)
        base58DecodeInto(textsOnes[i], out);
});
region(M.zs, () => {
    for (let i = 0; i < LENS.length; i++)
        base58DecodeInto(textsZs[i], out);
});
region(M.mixed, () => {
    for (let i = 0; i < LENS.length; i++)
        base58DecodeInto(textsMixed[i], out);
});
region(M.chk, () => {
    for (let k = 0; k < 3; k++)
        for (let i = 0; i < checkTexts.length; i++)
            base58CheckDecodeInto(checkTexts[i], out);
});

region(M.ez, () => {
    for (let i = 0; i < LENS.length; i++)
        Base58Encode(viewsZeros[i]);
});
region(M.ef, () => {
    for (let i = 0; i < LENS.length; i++)
        Base58Encode(viewsFfs[i]);
});
region(M.ea, () => {
    for (let i = 0; i < LENS.length; i++)
        Base58Encode(viewsAlt[i]);
});
region(M.em, () => {
    for (let i = 0; i < LENS.length; i++)
        Base58Encode(viewsMixed[i]);
});
region(M.ce, () => {
    for (let i = 0; i < LENS.length; i++)
        Base58CheckEncode(viewsMixed[i]);
});
region(M.bx, () => {
    for (let i = 0; i < LENS.length; i++)
        BaseXEncode(viewsMixed[i], HEX);
});

region(M.kz, () => {
    for (let i = 0; i < LENS.length; i++) {
        const n = lensZeros[i];
        if (n === 1) calTwo.slice(0, 1);
        else if (n > 1) calUnit.repeat(n);
    }
});
region(M.kf, () => {
    for (let i = 0; i < LENS.length; i++) {
        const n = lensFfs[i];
        if (n === 1) calTwo.slice(0, 1);
        else if (n > 1) calUnit.repeat(n);
    }
});
region(M.ka, () => {
    for (let i = 0; i < LENS.length; i++) {
        const n = lensAlt[i];
        if (n === 1) calTwo.slice(0, 1);
        else if (n > 1) calUnit.repeat(n);
    }
});
region(M.km, () => {
    for (let i = 0; i < LENS.length; i++) {
        const n = lensMixed[i];
        if (n === 1) calTwo.slice(0, 1);
        else if (n > 1) calUnit.repeat(n);
    }
});
region(M.kc, () => {
    for (let i = 0; i < LENS.length; i++) {
        const n = lensCheck[i];
        if (n === 1) calTwo.slice(0, 1);
        else if (n > 1) calUnit.repeat(n);
    }
});
region(M.kx, () => {
    for (let i = 0; i < LENS.length; i++) {
        const n = lensBasex[i];
        if (n === 1) calTwo.slice(0, 1);
        else if (n > 1) calUnit.repeat(n);
    }
});

region(M.pp, () => {
    for (let k = 0; k < 32; k++)
        Base58Encode(probeView);
});

region("##ALLOC-BEGIN decode-into-sliced-info##", () => {
    for (let i = 0; i < LENS.length; i++)
        base58DecodeInto(textsSliced[i], out);
});

print("test_base58_alloc: workload complete (" + LENS.length + " lengths, " +
      nNonempty + " non-empty, pool zone 0..512 dense)");
std.out.flush();
