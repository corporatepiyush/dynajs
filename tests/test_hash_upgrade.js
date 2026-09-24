/* test_hash_upgrade.js -- and (lane wc1-crypto).
 *
 * XXH3-64 one-shot, implemented from the xxHash 0.8 specification's
 * stripe constants (kSecret lives next to the implementation in
 * src/dyna-crypto.c) and pinned here against the REFERENCE IMPLEMENTATION:
 * the table below is generated from python xxhash 4.0.1, the project's own
 * binding, covering every length class the algorithm branches on (0, 1-3,
 * 4-8, 9-16, 17-128, 129-240, the 1024-byte long-input block boundary and
 * beyond) and three seeds. Also: XXHash64's {as} output opts.
 *
 * the streaming Hasher now covers the module's whole digest table
 * (SHA-3, Keccak, SHAKE, BLAKE2b/2s, BLAKE3). The oracle is the module's own
 * ONE-SHOT functions, which are themselves pinned to published KATs in
 * test_sha3.js / test_blake.js: streaming-in-chunks must equal the one-shot
 * digest for every chunking, because a sponge or tree walked differently is
 * a different hash function.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_hash_upgrade.js
 */
import * as h from "dyna:hash";

let n = 0, fails = 0;
function ok(c, msg) { n++; if (!c) { fails++; print("  FAIL " + msg); } }
function eq(a, b, msg) { ok(a === b, msg + " (got " + a + ", want " + b + ")"); }
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    ok(t, msg + " (did not throw)");
}
const data_of = (len) => { const b = new Uint8Array(len); for (let i = 0; i < len; i++) b[i] = (i * 89 + 31) % 256; return b; };
const tohex = (u) => Array.from(u, (b) => b.toString(16).padStart(2, "0")).join("");

// GENERATED: python xxhash 4.0.1 one-shot digests over data_of(n) (i*89+31 mod 256).
// [len, seed, XXH3_64, xxh64] as BigInt hex literals.
const XXH3_TABLE = [
  [0, 0x0n, 0x2d06800538d394c2n, 0xef46db3751d8e999n],
  [0, 0x1n, 0x4dc5b0cc826f6703n, 0xd5afba1336a3be4bn],
  [0, 0xdeadbeefn, 0x6676ee0cdb2228c2n, 0x1a49b996b6a42aa2n],
  [1, 0x0n, 0x5087bbed866d0den, 0x4c7f8d21e9dd7505n],
  [1, 0x1n, 0xc29f437e1e3de9d0n, 0x7134ac9e4744f43n],
  [1, 0xdeadbeefn, 0x8453007c5cc94484n, 0x9fab26ba108e2fbfn],
  [2, 0x0n, 0xc41ebe1a0db8b942n, 0x7469d98dcccab71cn],
  [2, 0x1n, 0xed853d509f036cc2n, 0xfce72fa3d1c4426n],
  [2, 0xdeadbeefn, 0xd7ddbba021cfc77bn, 0x453a90a031adb26n],
  [3, 0x0n, 0xbe206f7d3319c31en, 0x751fcc4bdf40d4e3n],
  [3, 0x1n, 0xb7a4411d79b7dc07n, 0xa60b6bc5ab25ef53n],
  [3, 0xdeadbeefn, 0x54829c3e3dbdf410n, 0xb6de68dce13b1799n],
  [4, 0x0n, 0xdec20f9d4b22f64bn, 0xd7e1939b781e0663n],
  [4, 0x1n, 0xa2279d9139348940n, 0x6b1e46a322d1386bn],
  [4, 0xdeadbeefn, 0xecde52db13f4d5f6n, 0x40034683865972d3n],
  [5, 0x0n, 0x10daf484f5fcc58dn, 0x3ba3d91a81c9605bn],
  [5, 0x1n, 0xe2c1edaf8e183aean, 0xb8b5844ecc15d976n],
  [5, 0xdeadbeefn, 0x2089e609163aba0n, 0xb841f6ea9a796e1cn],
  [7, 0x0n, 0xb334f5da66715a10n, 0xfb75e3143336fe43n],
  [7, 0x1n, 0xfc951d82683ff0fbn, 0xdc1b68add33ab8b9n],
  [7, 0xdeadbeefn, 0x7dd07ba3afb490fan, 0x6d41074ab27dd59fn],
  [8, 0x0n, 0xb7986ff4ee511bfn, 0xf7b5974fa2e92fbdn],
  [8, 0x1n, 0x92f583d820225075n, 0xd88a9cde6b78f9dfn],
  [8, 0xdeadbeefn, 0x7b042d0cc6051c6n, 0xe8888474195c504cn],
  [9, 0x0n, 0xe7eff1bc5f2ebd87n, 0xc8b7f5cf634dbbfbn],
  [9, 0x1n, 0x9b2d2768af271101n, 0x5d457c8890430ad0n],
  [9, 0xdeadbeefn, 0x36aeaf977a42c5b8n, 0xc9ea5ff862e782c4n],
  [13, 0x0n, 0x9a8b641770bf357n, 0xc4e48fbadadb81can],
  [13, 0x1n, 0x9d46a753a4e7acf8n, 0xeeb3720939ebba84n],
  [13, 0xdeadbeefn, 0x171ac0012105386an, 0x899e0cde75bdba64n],
  [16, 0x0n, 0xce815e2febb7ac5en, 0x42c5a8003f5957b4n],
  [16, 0x1n, 0x8d514fea5bf0478en, 0xb50cb0e3e8716011n],
  [16, 0xdeadbeefn, 0x36da94fde5e171a4n, 0x853f617aebb8a09bn],
  [17, 0x0n, 0x2d827de515b145e2n, 0x2fb8fd41eb24190n],
  [17, 0x1n, 0xe987c6a5ac2583cn, 0x5efc76261160783en],
  [17, 0xdeadbeefn, 0x170e2848938ce3e8n, 0x1f2a05ec41576611n],
  [31, 0x0n, 0xae5bbb8a442b7a14n, 0x5d119b9744f23905n],
  [31, 0x1n, 0x7e8fe48f61ae6ab6n, 0x795bca153f6b56n],
  [31, 0xdeadbeefn, 0x9ec691440ee4bc0cn, 0xdd7e097cf006d8f7n],
  [33, 0x0n, 0xb178f35ff2925c45n, 0x8647290307e09f89n],
  [33, 0x1n, 0x15d1357578d527c7n, 0x57ee7098be98552fn],
  [33, 0xdeadbeefn, 0x329317df16ee765dn, 0x68c036ad01ed3c1n],
  [63, 0x0n, 0xec8b3d65cb533bffn, 0x476d57782a116c7cn],
  [63, 0x1n, 0x5ddc069e55ddc382n, 0x4e6cc52fa9096708n],
  [63, 0xdeadbeefn, 0xf682357bf0a01742n, 0x73cfb3f4fab15015n],
  [64, 0x0n, 0xdbdaf85a13a7ef82n, 0xc69712ac5436bf0n],
  [64, 0x1n, 0xa66209783240d45n, 0x9598c4ed0f90e9a1n],
  [64, 0xdeadbeefn, 0xb21d6bfcd4cc2dfen, 0x82cc5309ae8e1d80n],
  [65, 0x0n, 0x6da7852be568efd0n, 0xcc69f01a64514163n],
  [65, 0x1n, 0xd475801285cae061n, 0x67a61fc9aa37f639n],
  [65, 0xdeadbeefn, 0x38122cef3b741450n, 0x979c24ae8662fbe6n],
  [96, 0x0n, 0x4aa30abace9cc1a5n, 0xe0a910e6b00151f6n],
  [96, 0x1n, 0xf91efcf6ff9f753an, 0x9ecfbc0bf6fd25b0n],
  [96, 0xdeadbeefn, 0x33058197d1696f54n, 0xd8660844933ea1fn],
  [127, 0x0n, 0x962ecd1d48664f48n, 0x95c870c3ffebc32fn],
  [127, 0x1n, 0x9e22cdfecbe89a53n, 0x51bfb385c2a9377an],
  [127, 0xdeadbeefn, 0xc7bb60e414b8ee67n, 0x580a9848d7ba548en],
  [128, 0x0n, 0x2d062fbcf9ea2c80n, 0xf903e95cdfce66ffn],
  [128, 0x1n, 0x70a1743e64ccf998n, 0xb0b7ba06b4f1b8d8n],
  [128, 0xdeadbeefn, 0xd7faf836e522c5n, 0xf506769318c57001n],
  [129, 0x0n, 0xfed5e0dfb70a2c51n, 0x5b4fbf3e3b77c27an],
  [129, 0x1n, 0x4df7233d0a8a1fd4n, 0x72e5b095a2f25e6bn],
  [129, 0xdeadbeefn, 0xf455e0f8f781434dn, 0x14b9f377544293e2n],
  [130, 0x0n, 0xdd001950077eed4fn, 0x97e2639bad1ca787n],
  [130, 0x1n, 0xcf5b3656d78ec48an, 0x84f51ce228a0a690n],
  [130, 0xdeadbeefn, 0xdc5410f896b95b6bn, 0xc9d49fd4ca34ae73n],
  [160, 0x0n, 0xc3c74a3d843feb8cn, 0x11aeb1385360bd58n],
  [160, 0x1n, 0x41f0c36120966be0n, 0xe742eef9711338dcn],
  [160, 0xdeadbeefn, 0xbe6229f64443a2f0n, 0x8e5e47b549168f17n],
  [223, 0x0n, 0x1510fe041ffd486en, 0xb873fa41f51339d8n],
  [223, 0x1n, 0x34e80b9f295369cbn, 0x248f208162dbf524n],
  [223, 0xdeadbeefn, 0xd6532c3e0bceeb56n, 0x3f53fcda4306963fn],
  [224, 0x0n, 0xbbe75498fcfbcd2n, 0x74a6c474c98c6334n],
  [224, 0x1n, 0x5c2c6d00570a8e55n, 0x83fdb092e0465eben],
  [224, 0xdeadbeefn, 0x9bb0318728d491d9n, 0xa8f53ec21f3025f2n],
  [239, 0x0n, 0x826fa25fa9b3d2fcn, 0x1a7f4e6d9911c178n],
  [239, 0x1n, 0xb392f4c2667affa4n, 0xa00ae864fdeac8d3n],
  [239, 0xdeadbeefn, 0x7451cfed00418b12n, 0x3daf09d8b15119d6n],
  [240, 0x0n, 0x9b9ab6e3a5cffc41n, 0x1ab48a45945bfc37n],
  [240, 0x1n, 0x1892ac6b803fe72an, 0x229978bf14f8cfe1n],
  [240, 0xdeadbeefn, 0x78b8dedf49db5b7dn, 0x532aa942902d139cn],
  [241, 0x0n, 0x2ceefda36b4291den, 0x7eb0edbee32b7ed0n],
  [241, 0x1n, 0xdd0398d2c00f895an, 0x5e4b022339a9bc5en],
  [241, 0xdeadbeefn, 0x42496d7929bc01ben, 0x61422aa225f6b2aen],
  [255, 0x0n, 0x632e1267a6dd89f6n, 0xd210f28a1756f04fn],
  [255, 0x1n, 0xeccb5c811996a5can, 0xd1f463ef68fe8feen],
  [255, 0xdeadbeefn, 0x2c37a279c7db1ec7n, 0x82bb55744dc20e85n],
  [256, 0x0n, 0x12e7c4f3fd094fc2n, 0x817abac34206db59n],
  [256, 0x1n, 0x61ec16a8b89dbd73n, 0xfa2a00593d0c47a0n],
  [256, 0xdeadbeefn, 0x16cc675cc32b1621n, 0x4c95e95338c937e7n],
  [300, 0x0n, 0x671498eb64b172f0n, 0x812d6cd43e7b6ba6n],
  [300, 0x1n, 0x864f12668e17405bn, 0x255d6e8c9b0a1453n],
  [300, 0xdeadbeefn, 0x60dfbe88945eb143n, 0xec73ba761bcabbe8n],
  [1023, 0x0n, 0x94d391e4555e1a0n, 0x75eb1e58a7a8d7e5n],
  [1023, 0x1n, 0x484ec0e75c2ee02fn, 0x9e99432b1195fb4dn],
  [1023, 0xdeadbeefn, 0x853ecf9a09f1f6c6n, 0xa32a41f80d65d6ban],
  [1024, 0x0n, 0x4418dc3edea818b8n, 0x9b80eb99f5f9e06n],
  [1024, 0x1n, 0xf957d1829e8c9994n, 0xf11976a9c0c6a16n],
  [1024, 0xdeadbeefn, 0x50ffc258c647fa43n, 0xeb4d0d2761610772n],
  [1025, 0x0n, 0xe953679c13bc857en, 0xabd6a8d752849c1cn],
  [1025, 0x1n, 0xe1197de218247c6fn, 0x635ba6516399a0a1n],
  [1025, 0xdeadbeefn, 0x5b35d3f750184c05n, 0x5016b1fb0400dan],
  [2048, 0x0n, 0x87d1118b2181f732n, 0x9b24438d9be1d674n],
  [2048, 0x1n, 0x867ffc5ad5d4f562n, 0x9886026d4c7d748dn],
  [2048, 0xdeadbeefn, 0x1dfd3f50fc97bc00n, 0x2d62d14e4fb590efn],
  [4096, 0x0n, 0xf132416de17d8217n, 0x55f5605eb2818b58n],
  [4096, 0x1n, 0x7d5a2abf3b970ffn, 0x53b189d2fd0db36an],
  [4096, 0xdeadbeefn, 0xd6af0a646a8d1b89n, 0xe7f3f6893f956e13n],
];

/* ----------: XXH3_64 against the reference table ---------- */
{
    for (const [len, seed, want3, want64] of XXH3_TABLE) {
        const d = data_of(len);
        eq(BigInt("0x" + h.XXH3_64(d, Number(seed))), want3,
           `XXH3_64(len=${len}, seed=${seed}) hex form`);
        // bytes form is little-endian and must reassemble to the same value
        const le = h.XXH3_64(d, Number(seed), { as: "bytes" });
        eq(le.length, 8, `XXH3_64(len=${len}) bytes length`);
        let v = 0n;
        for (let i = 7; i >= 0; i--) v = (v << 8n) | BigInt(le[i]);
        eq(v, want3, `XXH3_64(len=${len}, seed=${seed}) bytes reassembly`);
        // bigint form directly
        eq(h.XXH3_64(d, Number(seed), { as: "bigint" }), want3,
           `XXH3_64(len=${len}, seed=${seed}) bigint form`);
        // the XXH64 twin rides in the same table
        eq(BigInt("0x" + h.XXHash64(d, Number(seed))), want64,
           `XXHash64(len=${len}, seed=${seed}) still matches the reference`);
    }
    // default shape unchanged: hex string, both spellings, both modules' name
    eq(h.XXH3_64("").length, 16, "XXH3_64 default is a 16-char hex string");
    // the known constant: XXH3("") == 0x2d06800538d394c2 (the spec's own seed-0 empty input)
    eq(h.XXH3_64(""), "2d06800538d394c2", "XXH3_64 empty input constant");
    // opts validation
    throws(() => h.XXH3_64("x", 0, { as: "decimal" }), "unknown as value refuses");
    throws(() => h.XXHash64("x", 0, { as: "hex2" }), "XXHash64 unknown as value refuses");
    throws(() => h.XXH3_64("x", 1e308), "seed out of int64 range refuses");
    // unknown opts KEY is accepted-and-ignored? No: only `as` exists; strictness is
    // not claimed for this bag, but `as: null/undefined` must be the default
    eq(h.XXH3_64("x", 0, {}), h.XXH3_64("x"), "empty opts = default hex");
    eq(h.XXH3_64("x", 0, { as: undefined }), h.XXH3_64("x"), "undefined as = default hex");
}

/* ----------: the streaming Hasher covers the whole table ---------- */
{
    const algos = ["md5", "sha1", "sha224", "sha256", "sha384", "sha512",
                   "sha3_224", "sha3_256", "sha3_384", "sha3_512", "keccak256",
                   "shake128", "shake256", "blake2b", "blake2s", "blake3"];
    const oneshot = (algo, d) => {
        // the module's one-shot name for the algo, invoked on bytes
        const name = algo;
        const m = {
            md5: [h.MD5], sha1: [h.SHA1], sha224: [h.SHA224], sha256: [h.SHA256],
            sha384: [h.SHA384], sha512: [h.SHA512],
            sha3_224: [h.SHA3_224], sha3_256: [h.SHA3_256], sha3_384: [h.SHA3_384],
            sha3_512: [h.SHA3_512], keccak256: [h.Keccak256],
            shake128: [h.SHAKE128], shake256: [h.SHAKE256],
            blake2b: [h.BLAKE2b], blake2s: [h.BLAKE2s], blake3: [h.BLAKE3],
        };
        return m[name][0](d);
    };
    const msg = data_of(70);   // crosses 64- and 128-byte block boundaries? 70 crosses 64
    for (const algo of algos) {
        // streaming in one call equals the one-shot
        let hs = new h.Hasher(algo);
        hs.update(msg);
        ok(tohex(hs.digest()) === tohex(oneshot(algo, msg)),
           `Hasher("${algo}") one update == one-shot`);
        // chunked absorption (1,2,3,5,59-byte pieces) equals it too
        hs = new h.Hasher(algo);
        const cuts = [1, 2, 3, 5, 59];
        let off = 0;
        for (const c of cuts) { hs.update(msg.subarray(off, off + c)); off += c; }
        hs.update(msg.subarray(off));
        ok(tohex(hs.digest()) === tohex(oneshot(algo, msg)),
           `Hasher("${algo}") chunked updates == one-shot`);
        // digest() is non-destructive: further updates and re-digests agree
        hs.update(msg.subarray(0, 3));
        const twice = tohex(hs.digest());
        eq(twice, tohex(hs.digest()), `Hasher("${algo}") repeated digest stable`);
        // reset returns to the fresh state
        hs.reset();
        hs.update(msg);
        ok(tohex(hs.digest()) === tohex(oneshot(algo, msg)),
           `Hasher("${algo}") reset reuses the state`);
        eq(hs.algorithm, algo, `Hasher("${algo}").algorithm echoes the name`);
        hs.close();
    }

    /* length option: SHAKE and BLAKE take caller-chosen output; fixed digests refuse */
    {
        const sh = new h.Hasher("shake128", { length: 40 });
        sh.update("abc");
        eq(sh.digestSize, 40, "shake128 {length:40} digestSize");
        eq(sh.digestHex().length, 80, "shake128 40-byte hex length");
        ok(tohex(sh.digest()) === tohex(h.SHAKE128("abc", 40)),
           "shake128 streaming 40 == one-shot 40");
        eq(new h.Hasher("shake256", { length: 1 }).digest().length, 1, "shake256 length 1");
        eq(new h.Hasher("blake3", { length: 17 }).digest().length, 17, "blake3 length 17");
        eq(new h.Hasher("blake2b").digestSize, 64, "blake2b default digest is 64");
        eq(new h.Hasher("blake2s", { length: 20 }).digestSize, 20, "blake2s custom length");
        ok(tohex(new h.Hasher("blake2s", { length: 20 }).update("x").digest())
           === tohex(h.BLAKE2s("x", 20)), "blake2s 20 == one-shot 20");
        eq(new h.Hasher("sha3_256").digestSize, 32, "sha3_256 fixed 32");

        throws(() => new h.Hasher("sha256", { length: 10 }), "fixed digest refuses length (sha256)");
        throws(() => new h.Hasher("sha3_512", { length: 64 }), "fixed digest refuses length (sha3_512)");
        throws(() => new h.Hasher("keccak256", { length: 1 }), "fixed digest refuses length (keccak)");
        throws(() => new h.Hasher("shake128", { length: 0 }), "shake length 0 refuses");
        throws(() => new h.Hasher("shake128", { length: (1 << 20) + 1 }), "shake length over cap refuses");
        /* a NEGATIVE length is present nonsense, not an absent option: it
         * must throw, never collapse into the 32-byte default */
        throws(() => new h.Hasher("shake128", { length: -3 }), "shake length -3 refuses");
        throws(() => new h.Hasher("shake128", { length: -1 }), "shake length -1 refuses");
        throws(() => new h.Hasher("blake3", { length: -3 }), "blake3 length -3 refuses");
        throws(() => new h.Hasher("blake2b", { length: -1 }), "blake2b length -1 refuses");
        throws(() => new h.Hasher("sha256", { length: -3 }), "fixed digest refuses even a negative length");
        eq(new h.Hasher("shake128").digestSize, 32, "absent length still defaults to 32");
        throws(() => new h.Hasher("blake2b", { length: 65 }), "blake2b length 65 refuses");
        throws(() => new h.Hasher("blake2s", { length: 33 }), "blake2s length 33 refuses");
        throws(() => new h.Hasher("nope"), "unknown algorithm refuses");

        // a large SHAKE output (over the 200-byte stack scratch) flows through malloc
        const big = new h.Hasher("shake256", { length: 1000 });
        big.update(msg);
        ok(tohex(big.digest()) === tohex(h.SHAKE256(msg, 1000)),
           "shake256 1000-byte streaming == one-shot");
        // digestInto with a length beyond the old fixed maximum
        const buf = new Uint8Array(1000);
        big.reset().update(msg);
        eq(big.digestInto(buf), 1000, "digestInto with 1000-byte digest");
        eq(tohex(buf) === tohex(h.SHAKE256(msg, 1000)), true, "digestInto bytes correct");
        throws(() => big.digestInto(new Uint8Array(999)), "digestInto short buffer RangeError");
        big.close();
    }
}

/* ---------- adversarial: close() mid-chain ---------- */
{
    const hc = new h.Hasher("sha3_256");
    hc.update("abc");
    hc.close();
    throws(() => hc.update("d"), "update after close throws");
    throws(() => hc.digest(), "digest after close throws");
    throws(() => hc.digestHex(), "digestHex after close throws");
    throws(() => hc.digestInto(new Uint8Array(32)), "digestInto after close throws");
    throws(() => hc.reset(), "reset after close throws");
    throws(() => hc.algorithm, "algorithm getter after close throws");
    throws(() => hc.digestSize, "digestSize getter after close throws");
    hc.close();   /* idempotent: double close is safe (lifecycle contract) */
    eq(hc.closed, true, "closed stays true after double close");

    // close() between updates via a coercion's toString (the reentrancy attack
    // test_crypto.js runs on md5..sha512, replayed on a streaming SHA-3)
    const hostile = new h.Hasher("shake128", { length: 8 });
    hostile.update("ok");
    const evil = { toString() { hostile.close(); return "evil"; } };
    throws(() => hostile.update(evil), "toString closing this mid-coercion -> clean closed error");
    hostile.close();

    // PHC/verifyAsync parity is asserted in test_crypto_upgrade; here the
    // digest-size invariant one more time at both extrema
    eq(new h.Hasher("shake256", { length: 1 }).digestSize, 1, "shake256 length 1 floor");
    throws(() => new h.Hasher("shake256", { length: 0 }), "length 0 refused");
}

if (fails) {
    print("test_hash_upgrade: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_hash_upgrade failed");
}
print("test_hash_upgrade: " + n + " assertions, 0 failures");
