import { Random } from "dyna:random";

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
{
    for (const [seed, vec] of [[0n, VEC_SEED0], [(1n << 64n) - 1n, VEC_SEEDMAX],
                               [0x9e3779b97f4a7c15n, VEC_SEEDGOLDEN]]) {
        const r = new Random(seed);
        for (let i = 0; i < vec.length; i++)
            assert(r.nextU64() === vec[i], `golden vec seed=${seed} [${i}]`);
        const o = new Oracle(seed);
        for (let i = 0; i < vec.length; i++)
            assert(o.next() === vec[i], `oracle self-check seed=${seed} [${i}]`);
    }
}

{
    for (const seed of [0n, 42n, (1n << 64n) - 1n]) {
        const r = new Random(seed), o = new Oracle(seed);
        for (let i = 0; i < 20000; i++)
            assert(r.nextU64() === o.next(), `u64 oracle stream seed=${seed} @${i}`);
    }
    for (const seed of [0n, 42n, (1n << 64n) - 1n]) {
        const r = new Random(seed), o = new Oracle(seed);
        for (let i = 0; i < 5000; i++) {
            assert(same(r.nextU53(), o.u53()), `u53 oracle stream seed=${seed} @${i}`);
            const f = r.nextFloat(), d = o.dbl();
            assert(f === d, `float oracle stream seed=${seed} @${i}`);
        }
    }
    for (const seed of [0n, 42n, (1n << 64n) - 1n]) {
        const r = new Random(seed), o = new Oracle(seed);
        for (let i = 0; i < 5000; i++)
            assert(same(r.nextBounded(6), o.bounded(6n)), `bounded(6) stream seed=${seed} @${i}`);
        for (let i = 0; i < 500; i++)
            assert(same(r.nextBounded(9007199254740992), o.bounded(1n << 53n)),
                   `bounded(2^53) stream seed=${seed} @${i}`);
        for (let i = 0; i < 500; i++)
            assert(same(r.nextBounded(1n << 63n), o.bounded(1n << 63n)),
                   `bounded(2^63n) stream seed=${seed} @${i}`);
    }
    for (const seed of [0n, 42n, (1n << 64n) - 1n]) {
        for (const len of [128, 100, 7, 1]) {
            const arr = new Uint8Array(len);
            assert(new Random(seed).fill(arr) instanceof Random, `fill returns this len=${len}`);
            const want = new Oracle(seed).fill(len);
            for (let i = 0; i < len; i++) {
                assert(arr[i] === want[i], `fill byte [${i}] len=${len} seed=${seed}`);
            }
        }
    }
    {
        const r = new Random(42n), o = new Oracle(42n);
        for (let i = 0; i < 37; i++) { r.nextU64(); o.next(); }
        const st = r.getState();
        assert(st instanceof Uint8Array && st.length === 32, "getState is Uint8Array(32)");
        for (let i = 0; i < 4; i++) {
            let w = 0n;
            for (let j = 7; j >= 0; j--) w = (w << 8n) | BigInt(st[i * 8 + j]);
            assert(w === o.s[i], `getState word ${i} matches oracle state`);
        }
        const r2 = new Random(1);
        r2.setState(st);
        for (let i = 0; i < 5000; i++)
            assert(r2.nextU64() === o.next(), `setState resume @${i}`);
        st[0] = 255; st[31] = 42;
        assert(r2.nextU64() === o.next(), "mutating a snapshot does not touch the generator");
    }
}

{
    assert(new Random(42).nextU64() === new Random(42n).nextU64(), "42 == 42n seed");
    assert(new Random(1.5).nextU64() === new Random(1).nextU64(), "1.5 truncates to 1");
    assert(new Random(NaN).nextU64() === new Random(0).nextU64(), "NaN seeds as 0");
    assert(new Random(-0).nextU64() === new Random(0).nextU64(), "-0 seeds as 0");
    assert(new Random(null).nextU64() === new Random(0).nextU64(), "null seeds as 0");
    assert(new Random(true).nextU64() === new Random(1).nextU64(), "true seeds as 1");
    assert(new Random("7").nextU64() === new Random(7).nextU64(), "'7' seeds as 7");
    assert(new Random(-5).nextU64() === new Random((1n << 64n) - 5n).nextU64(), "-5 == 2^64-5");
    assert(new Random().nextU64() !== new Random().nextU64(), "unseeded instances differ");
}

{
    const r = new Random(1);
    for (const bad of [0, -7, 0.5, 6.5, NaN, Infinity, -Infinity, 9007199254740994, 1e30]) {
        let threw = false;
        try { r.nextBounded(bad); } catch (e) { threw = e instanceof RangeError; }
        assert(threw, `nextBounded(${bad}) throws RangeError`);
    }
    for (const bad of [0n]) {
        let threw = false;
        try { r.nextBounded(bad); } catch (e) { threw = e instanceof RangeError; }
        assert(threw, "nextBounded(0n) throws RangeError");
    }
    {
        let threw = false;
        try { r.nextBounded(); } catch (e) { threw = e instanceof RangeError; }
        assert(threw, "nextBounded() with no argument throws RangeError");
    }
    assert(typeof r.nextBounded(6) === "number", "Number bound -> Number");
    assert(typeof r.nextBounded(6n) === "bigint", "BigInt bound -> BigInt");
    assert(r.nextBounded(9007199254740992) < 9007199254740992, "bounded(2^53) < 2^53");
    assert(new Random(7).nextBounded(-5n) === new Random(7).nextBounded((1n << 64n) - 5n),
           "-5n aliases 2^64-5n");
    assert(new Random(9n).nextBounded(1) === 0 && new Random(9n).nextBounded(1n) === 0n,
           "bounded(1) is constant 0");
    for (let i = 0; i < 1000; i++)
        assert(r.nextBounded((1n << 64n) - 1n) < (1n << 64n) - 1n, "bounded(2^64-1) < bound");
    let threw = false;
    try { Random.prototype.nextU64.call({}); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "nextU64.call({}) throws TypeError");
}

{
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
}

{
    const r = new Random(11);
    r.nextU64();
    const st = r.getState();
    for (const len of [0, 1, 31, 33, 64]) {
        let threw = false;
        try { new Random(1).setState(new Uint8Array(len)); } catch (e) { threw = e instanceof RangeError; }
        assert(threw, `setState(${len} bytes) throws RangeError`);
    }
    const w = new Int32Array(8);
    w.set([1, 2, 3, 4, 5, 6, 7, 8]);
    assert(new Random(1).setState(w) === undefined, "setState(Int32Array(8)) accepted");
    let threw = false;
    try { new Random(1).setState(new DataView(new ArrayBuffer(32))); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "setState(DataView) throws TypeError");
    threw = false;
    try { new Random(1).setState(new Uint8Array(32)); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "setState(all zeros) throws RangeError");
    const padded = new Uint8Array(64);
    padded.set(st, 16);
    const r3 = new Random(11);
    r3.nextU64();
    r3.setState(padded.subarray(16, 48));
    assert(r3.nextU64() === r.nextU64(), "setState(offset subarray view) resumes exactly");
    const a = new Random(1), b = new Random(2);
    const sa = a.getState();
    a.nextU64(); b.nextU64();
    const sa2 = a.getState();
    assert(sa[0] !== sa2[0] || sa[1] !== sa2[1], "generators are independent");
    const c = new Random(99);
    c.setState(r.getState());
    assert(c.nextU64() === r.nextU64(), "setState clones the stream across instances");
}

{
    class R extends Random {}
    const a = new R(9);
    assert(a instanceof R && a instanceof Random, "subclass instance is branded R and Random");
    assert(!(new Random(9) instanceof R), "plain instances are not branded R");
    assert(Object.getPrototypeOf(a) === R.prototype, "subclass prototype is R.prototype");
    assert(a.nextU64() === new Random(9).nextU64(), "subclass streams are still correct");
}

{
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
}

print("test_random_audit: all tests passed (" + n + " assertions)");
