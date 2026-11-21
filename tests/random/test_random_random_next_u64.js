// covers: dyna:random.Random.nextU64
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_random_random_next_u64");
import { Random } from "dyna:random";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}

__kitS.section("42 and 42n produce same stream @", () => {
    const a = new Random(42);
    const b = new Random(42n);
    {
        for (let i = 0; i < 100; i++)
            assert(a.nextU64() === b.nextU64(), "42 and 42n produce same stream @" + i);
    }
});

__kitS.section("distinct seeds should rarely collide", () => {
    const a = new Random(1), b = new Random(2);
    {
        let same = 0;
        for (let i = 0; i < 50; i++) if (a.nextU64() === b.nextU64()) same++;
        assert(same < 3, "distinct seeds should rarely collide (" + same + ")");
    }
});

__kitS.section("no close(): the whole state is one integer", () => {
    const r = new Random(1);
    assert(r.close === undefined, "no close(): the whole state is one integer");
    assert(r.closed === undefined, "no closed getter");
    assert(r[Symbol.dispose] === undefined, "not a disposable");
    for (let i = 0; i < 100; i++) r.nextU64();
    assert(typeof r.nextU64() === "bigint", "still usable after arbitrary use");
});

__kitS.section("getState returns Uint8Array(32)", () => {
    const r = new Random(42n);
    const st = r.getState();
    assert(st instanceof Uint8Array && st.length === 32,
           "getState returns Uint8Array(32)");
    const pulled = [];
    for (let i = 0; i < 10; i++) pulled.push(r.nextU64());
    r.setState(st);
    for (let i = 0; i < 10; i++)
        assert(r.nextU64() === pulled[i], "setState replays the stream @" + i);
    const r2 = new Random(7);
    r2.setState(st);
    for (let i = 0; i < 10; i++)
        assert(r2.nextU64() === pulled[i], "state transfers across generators @" + i);
    const padded = new Uint8Array(64);
    padded.set(st, 16);
    r.setState(padded.subarray(16, 48));
    assert(r.nextU64() === pulled[0] && r.nextU64() === pulled[1],
           "setState accepts an offset subarray view");
    let threw = false;
    try { r.setState(new Uint8Array(32)); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "setState(all zeros) throws RangeError");
    threw = false;
    try { r.setState(new Uint8Array(31)); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "setState(31 bytes) throws RangeError");
});

__kitS.section("golden u64 #", () => {
    const want = [12966619160104079557n, 9600361134598540522n, 10590380919521690900n,
                  7218738570589545383n, 12860671823995680371n, 2648436617965840162n,
                  1310552918490157286n, 7031611932980406429n];
    const r = new Random(1n);
    for (let i = 0; i < want.length; i++)
        assert(r.nextU64() === want[i], "golden u64 #" + i + " of seed 1n");
    const r2 = new Random(1n);
    const b = new Uint8Array(4);
    r2.fill(b);
    assert(b[0] === 197 && b[1] === 16 && b[2] === 199 && b[3] === 15,
           "golden first 4 fill() bytes of seed 1n");
});

__kitS.section("control pair identical pre-jump @", () => {
    const a = new Random(7), b = new Random(7);
    const c = new Random(7);
    for (let i = 0; i < 100; i++)
        assert(a.nextU64() === c.nextU64(), "control pair identical pre-jump @" + i);
    const ret = b.jump();
    assert(ret === b, "jump() returns this");
    assert(a.getState().join(",") !== b.getState().join(","),
        "jump advances the state (getState differs)");
    const seen = new Set();
    for (let i = 0; i < 100000; i++) seen.add(a.nextU64().toString());
    let disjoint = true;
    for (let i = 0; i < 100000; i++)
        if (seen.has(b.nextU64().toString())) disjoint = false;
    assert(disjoint, "jumped pair produce 100k-draw disjoint subsequences");
    const d1 = new Random(9), d2 = new Random(9);
    d1.jump(); d2.jump();
    assert(d1.nextU64() === d2.nextU64(), "jump is deterministic per seed");
    const e = new Random(3);
    e.jump(); e.jump();
    const f = new Random(3);
    f.jump();
    assert(e.getState().join(",") !== f.getState().join(","),
        "jump twice advances further than once");
    const g = new Random(5);
    assert(g.longJump() === g, "longJump() returns this");
});
__kitS.end();

print("test_random: all tests passed (" + n + " assertions)");
