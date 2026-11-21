// flags: --std
// covers: dyna:hash.BLAKE3Hex
// BLAKE3 inputs over 128 KiB hash aligned power-of-two subtrees on dyn-par workers (parts of 64 chunks,
// never the root: input always remains after the last subtree). Oracles:
//  0. the OFFICIAL BLAKE3 test vectors, vendored as tests/crypto/blake3_official_vectors.json from
//     https://github.com/BLAKE3-team/BLAKE3/blob/master/test_vectors/test_vectors.json (CC0-1.0 OR
//     Apache-2.0): 35 lengths x hash / keyed_hash / derive_key = 105 values (derive_key compared on its
//     32-byte prefix: our derive-key output length is fixed at 32);
//  1. for the parallel range the official vectors do not reach (they stop at 100 KiB): vectors from an
//     independent pure-Python BLAKE3 written from the specification (tests/crypto/gen_blake3_par_vectors.py),
//     which asserts all 105 official values before printing, so its large vectors are cross-validated;
//     input byte i % 251, plain, keyed, derive-key, XOF 131 and 1000;
//  2. the streaming Hasher fed at awkward split points (1, 1023, 1024, 1025, 64-chunk boundaries +-1,
//     then one large update), so subtrees start at a non-zero absolute chunk counter;
//  3. a child with --io-threads 0 (serial) and children whose helper spawns fail after 0/1/2 threads
//     (test hook DYNAJS_PAR_SPAWN_LIMIT) agree;
//  4. sys.parStats proves helpers ran above the crossover and none below it.
// DYNAJS_PAR_BIG=1 adds the inputs over 2 MiB (default run stays short; the crossover sizes are always on).
import * as std from "std";
import { BLAKE3Hex, Hasher } from "dyna:hash";
import { args, Exec, parStats } from "dyna:sys";
import { suite } from "../lib/kit.js";

const MiB = 1 << 20;
const BIG = std.getenv("DYNAJS_PAR_BIG") === "1";

const ALL_VECTORS = [
    [131071, "plain", 32, "de433db299ce5940eb72f08f509f90fa93e8b8c38e26c927310b4f8b98e2f33c"],
    [131072, "plain", 32, "306baba93b1a393cbd35172837c98b0f59a41f64e1b2682ae102d8b2534b9e1c"],
    [131073, "plain", 32, "f837d4254d24ba3d50fe3743d46e4af6db5f5d6ab0469197d94e7ba1e906c4d8"],
    [132097, "plain", 32, "d3bffd4815c1159749bd0b24688731909d251e76b4baede86dd151475e481d53"],
    [196609, "plain", 32, "24af087e4d878227c008c68e1813d2c10d621f9ffcca0160cf11b9bc5f19a532"],
    [1048575, "plain", 32, "f32b849d19684c18c138cf13e29dbadc776f4bc2a56b477680ef546b39ac3f71"],
    [1048576, "plain", 32, "74cb441fd087764ca9c3694da742ebe30cbeb3060a17009ca81825c7a8d10343"],
    [1048577, "plain", 32, "2f053cd7472cf0cd2f9adaf45c1180255b91b9a865404a63671a0ee5f792ed33"],
    [1049600, "plain", 32, "59f733f7a6508b55eb572977d6b5b344769021d8689804460bbbaad07da9688a"],
    [1049601, "plain", 32, "860f19b5fefff01454de342be87a20059449529116a20fb22a21da665aafa071"],
    [2097153, "plain", 32, "52dc212cb4cc61cb94d25bd7b1d47b256e4c3a6d68956df50c235c37a2aeacd7"],
    [3145733, "plain", 32, "a7bb55bed0c04f58879d1fc1cafb27e14e931f4411fe63baf5b2d5a60357bffb"],
    [4195329, "plain", 32, "5e854331c8e64ba0cd85e209bffff9c882575fd2e24ada0ca34fa7afbc1a4eb6"],
    [6291456, "plain", 32, "60cd1e4e80a3f746015c4453978d0a7f901e0764b2931bbc9c44ab7b18fc7684"],
    [2097153, "keyed", 32, "e7c0a70fe93439f6ae5ce9cd05a2dcf1936d2a0e8b0ac93b95368d4fd598fdf9"],
    [2097153, "derive", 32, "1e9d880bbf653890e1db50a56e0adf35d0afbdd910f5f3719146b4e67f4a78d0"],
    [2097153, "xof", 131, "52dc212cb4cc61cb94d25bd7b1d47b256e4c3a6d68956df50c235c37a2aeacd70cbd9edc61aef1667ef597b28075ac3255cc95c0e1d39a660f6d8761d36f405e5275c8a21c65c9d823d618e8e10856a67bf89c3ed21a260702d9394a1fd28d15594c49004ba8e63341d7a8bb0589d8fb25e819cabe4734bf18c6a8e8135c9a9950cce2"],
    [2097153, "keyed-xof", 1000, "e7c0a70fe93439f6ae5ce9cd05a2dcf1936d2a0e8b0ac93b95368d4fd598fdf9d158b4d8666dfd62f2591795803f09be0813a492cedeaf8e704e60056444cd3aed0a7c520c09fa1cfd502abad98a2dfe0a5b43d5fccca8b27ce6d58579540b9f3c008ccae85f13db466f43594765500cfae547b50adf4b3af87f292a203688768bbcd3d42b724da8c8964bff0abd26a7f766535c428dcdb1c1a5a5d8cdee0815a483350dee0dba6a24869ec160eafb8e9ed8f0b1b299ddbe591cb088908f6485906c35af7ed2d057d3bb84123c056761e590efdde6659bc180ab2cec5a38c814cc3c22eb06d459600cbd00931829363fe0eff5e6c387d6396b425358d9aec02aa26c33a202b295463bc082246c5325d17cbab36c846aa86e6d4d8235937f2a71191b8c566e70642c2fb245ced38471f5167b57f4cddb70a3c1046b2698cae206a8e8cc62b3428fb3447df132cee351b9be9f23c6e6ab9ceb0be7488bc50483fa37166fe20f83179670c59b7c0e507cc66fbbc674ecf4b995bff9c28a8ffcdea50f2d916b802dc473d481e559972ce61bc09c034ee74984ff89ce16d99287d38afe51ae514c90f76cca4a0f97833e134bcf94b8275b0603c4e4c8775b7f190406582f4601982993542b026d83aaf1b7ac242b0f21abd549dfe7fbca197be9bcbdc88775f8aefd8308d51d1f8309507e26305b838eba83c7ab1853631aeac617a6e46e85a7ecface64bee64a225e737fd86f170c96dbcc4b12bdfc784377197e19f399b1664260d332fe32fce611ab054a8b47532f54a7c83a3471d53581feca8997e0ef8e4126668c257a3629b0b37a53661062cb3e10d7f6d904b437c7bede21d12c70fc199bdb49ea4ff868815c7d785aa5ea1bbcc813019e5c25f6b1537bdbc04c5313563187c64f823b71cb79ec0aeddafde4fb5e53ae850d293c5dd78c5f1557d6188a5aec485ee47fa80c06b122f280bb7f4ae634e7148857cc7a60f156ffd6b3aaae1a9383e79a5ba45e68c5d383a9816d2fb82df373e9721b0785a13637f07785c179a63767966fe920ab6027e1e16237cf3b7d91d79969a9974feedd2d6283fcf53f81f6fb2f6a5700da18a8b1a774f545d64b7caeaba2b879f1848a735e0e6cb9aa69e1d364a0d9a8f8921546ad1128cd4ad5b096df7a73dbefdc6315c0713e09602fa4bd361da0adda94c7bbee30810d6da8c4e1296edb42edc33fd547d62df0c245780408e8fbe740a58e824fb742aa5a93174725c63f7dfa12c0ed63a67469f9f5b1c2eb5d0d3277fb09af308de8eb04e0bbcdbea8322002dd458a86ad745db1683fe5db9a95c80d6abf703b49410786a623fdd74d23f9970199b6163325d0a1e40b24ad1ec97862bdf60fd39fc0deb3efa41d33becf078e035716c95addb5d69dcc"],
    [3146752, "keyed", 32, "c544a05fee29cd54166a656e1d988d252b9e4fd5d866de2c40f413f9c22b6412"],
    [3146752, "derive", 32, "3bcf28513cfc4713d92a85a565e0dea332b9850fd8d11948a5d54a010c922fc9"],
    [3146752, "xof", 131, "f50b9c7a909d3a613ef6d072d3a17eb3818537b26bd1d708094ec887b344ea3782721407747fa18556c01400b6ca5d542da0ef01f46ad8738bad73744f24b22337af0954a87ba28566c9193dfda5be16af9687345dd59775238831cf8645f2355d2ed6db6a11b460b9b249d87c872d94002e8d706b89750f6e6486fde78ee8892c7d48"],
    [3146752, "keyed-xof", 1000, "c544a05fee29cd54166a656e1d988d252b9e4fd5d866de2c40f413f9c22b6412a757b41facbf64cf3d429c0a39c907eb42a7872907fa0135a2237477e31103b80a8b4b499dadea620d077cd4cda6f56777e67c08dfc04186564d3f0c332441458ce7e7fc1f9d9dbdd39212f624020e99e9f56b7eea7235e50af43f7a5bed216d3e44d46c4023fea72da5764762b81274eae953b1e5911d051b8d38915bc96b166d92678b378c08b7cab7523db049d6b0cbc19b4cc361dfbbc20112acce4dbfa58f9159640ab8757d4ace0c5cfca64e3412259710a0cc850c66a4097198f3de50fc70a7aff886051412d30974ad852f862e9eba18c6aca8f9ca8b68bebaa2c73fa34d8ebfdb394658efb5f53e13df0418f9b8d42337aa34d14498afac848ba373e31ec4cc01cec98c06a1e1fba5cfa29fcf57fea58e81d133470ac03139cd9ed0df27e284d5d64c5d0885bd2e1811862303ccafaf76e8ec69d1ac442cf10fb725d857b0090f78bbb5074621b166a11d3a1e30ca72b840bf22e20661cbccce80cbbec558e02cb37cf54790bb5bfe1b94816ecdaa643eef695b0f011934d2e1ca0676d3b29a602ea0d2932087b4706de905205e9d0f06b413de9d5a3f0a5af490b865f27da0072966cd75030626d3ed9bdf24540ce7704b790df856d1f29d66bb9cdecfc4e072fa2bc0c9599995274872de892e018371e574347c90795debfe4bd73ee56b991fa4a49cba9216a1cea8a32c0a972a776c3405e85f00f330aa17a878690d810ac2a01736aadb639c4f693427c1a4121f484ccfa60ab5bd82fe76569abbfe7298bed1c15e4fad397378fff8960e296df1b9af1d4986c2146202a39cd3ef962d699613bf1ac95307f25bbcbce40cf53b1e68c7ac9b4545d15603c7d44e73d62600b8e2fe8ab5f41b779f76cec41f68f547b431e59de44310c47a7990076a0ef585ed512f213fe431523ec135c03264f8490313242f22920f9552e86d8cce636e8172864ff56962e588888ee4c9e21e3da0e157d0e4a611bd91cbaf069780a15e7e230307f11c75ead6bc91967e3fb71fe2f0446d6caff4ee6754f08dc39414b9c295778e9f8626a1256969e00cac99dc5b71196f9db7fe72296d293cc58c966bb7e63b7919601913df2ec08841b6616f664306f75e74cda3b5fd29f4a7d17cb2449b385da383d6f9d572c523e1f1cee7108eff60a06a8f1b85f70376b8534f6741c3c5b79ef38002103a6be12f37c6aa0ff909dfca11019af97313f67956288fd519fe933ffbc32f64c3ea739f4f8d173dfffa98c074306aafb744d1aa0b38a30c5710f195f8972a3dd82d53b7a167f1532397d19b8ff2ebab8530cbf1944486a86f9fc17c42e64fcf0b9ffa32db35fa54011676ee6c116d5fbb411c6c90a3eaf6b89b74f5"],
];
const VECTORS = ALL_VECTORS.filter((v) => BIG || v[0] <= 2 * MiB + 1);
const KEY = new TextEncoder().encode("whats the Elephant and Castle my");
const CTX = "BLAKE3 2019-12-27 16:29:52 test vectors context";
const big = new Uint8Array((BIG ? 6 : 2) * MiB + 4096);
for (let i = 0; i < big.length; i++) big[i] = i % 251;
const pat = (n) => big.subarray(0, n);
const run = (n, mode, len) => {
    if (mode === "plain") return BLAKE3Hex(pat(n), len);
    if (mode === "keyed") return BLAKE3Hex(pat(n), { key: KEY });
    if (mode === "derive") return BLAKE3Hex(pat(n), { context: CTX });
    if (mode === "xof") return BLAKE3Hex(pat(n), len);
    return BLAKE3Hex(pat(n), { key: KEY, length: len });
};
const SPLITS = [[1], [1023], [1024], [1025], [64 * 1024 - 1], [64 * 1024 + 1], [256 * 1024 - 1], [256 * 1024 + 1], [1, 256 * 1024], [512 * 1024 + 3, 1024], [1024, 1024, 1025], [1024 * 1024 + 1024]];
function streamed(n, cuts) {
    const h = new Hasher("blake3");
    let at = 0;
    for (const c of cuts) { h.update(pat(n).subarray(at, at + c)); at += c; }
    h.update(pat(n).subarray(at));
    const d = h.digestHex();
    h.close();
    return d;
}
const SN = BIG ? 4 * MiB + 1025 : 2 * MiB + 1;
const all = () => VECTORS.map(([n, mode, len]) => run(n, mode, len)).concat(SPLITS.map((c) => streamed(SN, c)));

if (scriptArgs[1] === "--child") {
    const before = parStats();
    const d = all();
    const after = parStats();
    print(JSON.stringify({ d, spawned: after.spawned - before.spawned, failures: after.spawnFailures - before.spawnFailures }));
    std.exit(0);
}
const BIN = args()[0], SELF = scriptArgs[0];
function child(flags, env) {
    const r = Exec(BIN, [...flags, "--std", SELF, "--child"], { timeoutMs: 120000, encoding: "utf8", env: Object.assign(BIG ? { DYNAJS_PAR_BIG: "1" } : {}, env || {}) });
    if (r.code !== 0) throw new Error("child " + flags.join(" ") + " exited " + r.code + ": " + String(r.stderr).slice(0, 300));
    return JSON.parse(String(r.stdout).trim().split("\n").pop());
}

const t = suite("blake3-par");

t.test("official BLAKE3 test vectors: 35 lengths x hash, keyed_hash, derive_key", (a) => {
    const d = JSON.parse(std.loadFile(SELF.replace(/[^/]*$/, "") + "blake3_official_vectors.json"));
    a.must(d && d.cases && d.cases.length === 35, "vendored official vector file loads with 35 cases");
    const okey = new TextEncoder().encode(d.key);
    for (const c of d.cases) {
        const input = pat(c.input_len);
        a.eq(BLAKE3Hex(input, c.hash.length / 2), c.hash, "hash " + c.input_len);
        a.eq(BLAKE3Hex(input, { key: okey, length: c.keyed_hash.length / 2 }), c.keyed_hash, "keyed " + c.input_len);
        a.eq(BLAKE3Hex(input, { context: d.context_string }), c.derive_key.slice(0, 64), "derive " + c.input_len);
    }
});

t.test("independent-reference vectors: plain, keyed, derive, XOF", (a) => {
    for (const [n, mode, len, want] of VECTORS) a.eq(run(n, mode, len), want, n + " " + mode + " " + len);
});

t.test("helpers run above the crossover and not below it", (a) => {
    const s0 = parStats();
    if (s0.budget === 0) a.skip("single-CPU host: no helper budget");
    BLAKE3Hex(pat(128 * 1024));
    const s1 = parStats();
    a.eq(s1.spawned, s0.spawned, "128 KiB (two parts, the second is the root's right child) stays serial");
    BLAKE3Hex(pat(2 * MiB));
    const s2 = parStats();
    a.ok(s2.spawned > s1.spawned, "2 MiB spawned helpers: " + (s2.spawned - s1.spawned));
    streamed(SN, [1]);
    a.ok(parStats().spawned > s2.spawned, "a stream misaligned by one byte still reaches the parallel path");
    a.eq(parStats().live, 0, "no helper outlives the call");
});

t.test("serial and spawn-limited children agree", (a) => {
    const here = all();
    const serial = child(["--io-threads", "0"]);
    a.eq(serial.spawned, 0, "--io-threads 0 spawns no helper");
    a.deep(serial.d, here, "--io-threads 0 equals the parallel run");
    for (const lim of ["0", "1", "2"]) {
        const c = child([], { DYNAJS_PAR_SPAWN_LIMIT: lim });
        a.deep(c.d, here, "spawn limit " + lim);
        if (parStats().budget > Number(lim)) a.ok(c.failures > 0, "spawn limit " + lim + " injected failures (" + c.failures + ")");
    }
}, { timeoutMs: 120000 });

await t.run();
