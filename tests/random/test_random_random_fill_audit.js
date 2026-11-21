// covers: dyna:random.Random.fill
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_random_random_fill_audit");
import { Random } from "dyna:random";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}

const M64 = (1n << 64n) - 1n;
function rotl(x, k) { return ((x << BigInt(k)) & M64) | (x >> BigInt(64 - k)); }
function splitmix64(st) {
    st.s = (st.s + 0x9e3779b97f4a7c15n) & M64;
    let z = st.s;
    z = ((z ^ (z >> 30n)) * 0xbf58476d1ce4e5b9n) & M64;
    z = ((z ^ (z >> 27n)) * 0x94d049bb133111ebn) & M64;
    return (z ^ (z >> 31n)) & M64;
}
class Oracle {
    constructor(seed) {
        const st = { s: seed & M64 };
        this.s = [splitmix64(st), splitmix64(st), splitmix64(st), splitmix64(st)];
    }
    next() {
        const s = this.s;
        const result = rotl((s[1] * 5n) & M64, 7) * 9n & M64;
        const t = (s[1] << 17n) & M64;
        s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
        s[2] ^= t; s[3] = rotl(s[3], 45);
        return result;
    }
    u53() { return this.next() >> 11n; }
    dbl() { return Number(this.next() >> 11n) / 9007199254740992.0; }
    bounded(bound) {
        const threshold = (1n << 64n) % bound;
        for (;;) {
            const v = this.next();
            if (v >= threshold) return v % bound;
        }
    }
    fill(n_) {
        const out = new Uint8Array(n_);
        let i = 0;
        while (i < n_) {
            const v = this.next();
            for (let j = 0; j < 8 && i < n_; j++, i++)
                out[i] = Number((v >> BigInt(8 * j)) & 0xffn);
        }
        return out;
    }
}

const same = (a, b) => a === b || String(a) === String(b);

const VEC_SEED0 = [
    11091344671253066420n, 13793997310169335082n, 1900383378846508768n, 7684712102626143532n,
    13521403990117723737n, 18442103541295991498n, 7788427924976520344n, 9881088229871127103n,
    15781505947799885617n, 16949938600482740797n, 2108416074180405844n, 1240209487116192693n,
    1967799970308132508n, 12079539854699322239n, 9150657576430337180n, 5466973851375020728n];
const VEC_SEEDMAX = [
    10328197420357168392n, 14156678507024973869n, 9357971779955476126n, 13791585006304312367n,
    10463432026814718762n, 13498236496097551653n, 6831296623176769502n, 14161350843019729634n,
    11558284878126271842n, 11331410254202166410n, 4742038592102647401n, 809304973552138745n,
    8839074049581579390n, 8873532491084022193n, 3525529801700352095n, 14324370401275619504n];
const VEC_SEEDGOLDEN = [
    4768932952251265552n, 16168679545894742312n, 6487188721686299062n, 86499648889209533n,
    16455235402234500827n, 4306002562074487087n, 6917561557383370982n, 11578438031395272546n,
    11395172309285009232n, 18192772786079361596n, 15289854319975014978n, 6740795971359405421n,
    7068733013270630549n, 13143835648259585293n, 3250805849393379909n, 8738446437910061100n];

__kitS.section("fill(subarray) touches only the view's bytes", () => {
    const r = new Random(3);
    for (const T of [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array,
                     Int32Array, Uint32Array, Float32Array, Float64Array,
                     BigInt64Array, BigUint64Array]) {
        const a = new T(24);
        assert(r.fill(a) === r, `fill(new ${T.name}(24)) returns this and accepts it`);
    }
    const base = new Uint8Array(16);
    new Random(5).fill(base.subarray(4, 12));
    let outside = 0;
    for (let i = 0; i < 16; i++) if (i < 4 || i >= 12) outside += base[i] === 0 ? 0 : 1;
    assert(outside === 0, "fill(subarray) touches only the view's bytes");
    assert(new Random(5).fill(new Uint8Array(0)) instanceof Random, "fill(zero-length) ok");
    for (const bad of [undefined, {}, [1, 2, 3], "xxxx"]) {
        let threw = false;
        try { new Random(1).fill(bad); } catch (e) { threw = e instanceof TypeError; }
        assert(threw, `fill(${String(bad)}) throws TypeError`);
    }
    for (const bad of [new DataView(new ArrayBuffer(16)), new ArrayBuffer(16)]) {
        let threw = false;
        try { new Random(1).fill(bad); } catch (e) { threw = e instanceof TypeError; }
        assert(threw, `fill(${bad.constructor.name}) throws TypeError`);
    }
    const d = new Uint8Array(8);
    try { d.buffer.transfer(); } catch (e) {  }
    if (d.byteLength === 0) {
        let threw = false;
        try { new Random(1).fill(d); } catch (e) { threw = true; }
        assert(threw, "fill(detached) throws");
    }
});

__kitS.section("nextFloat outside [0,1)", () => {
    const r = new Random(1234567);
    {
        const B = 10, N = 1000000;
        const counts = new Array(B).fill(0);
        for (let i = 0; i < N; i++) counts[r.nextBounded(B)]++;
        let chi2 = 0;
        for (let i = 0; i < B; i++) { const e = N / B, d = counts[i] - e; chi2 += d * d / e; }
        assert(chi2 > 1 && chi2 < 30, `chi2(df=9) sane: ${chi2.toFixed(3)}`);
    }
    {
        const M = 200000;
        let sum = 0, gridOk = true;
        for (let i = 0; i < M; i++) {
            const v = r.nextFloat();
            sum += v;
            if (!(v >= 0 && v < 1)) throw new Error("nextFloat outside [0,1)");
            if ((v * 9007199254740992) % 1 !== 0) gridOk = false;
        }
        assert(gridOk, "nextFloat on the 2^-53 grid");
        const mean = sum / M;
        assert(mean > 0.49 && mean < 0.51, `nextFloat mean ~0.5 (${mean.toFixed(4)})`);
    }
    {
        let max = 0;
        for (let i = 0; i < 200000; i++) { const v = r.nextU53(); if (v > max) max = v; }
        assert(max < 9007199254740992, `nextU53 < 2^53 (max seen ${max})`);
    }
});
__kitS.end();

print("test_random_audit: all tests passed (" + n + " assertions)");
