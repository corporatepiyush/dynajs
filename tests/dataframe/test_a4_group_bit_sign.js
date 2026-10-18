import { DataFrame } from "dyna:dataframe";

let n = 0, fails = 0;
function assert(c, msg) {
    n++;
    if (!c) {
        fails++;
        print("FAIL: " + msg);
    }
}

{
    const df = new DataFrame({ k: ["g"], v: new Uint32Array([2147483648]) });
    const r = df.GROUP_BIT_OR("k", "v");
    assert(r.values[0] === 2147483648, "GROUP_BIT_OR keeps the high bit unsigned (got " + r.values[0] + ")");
    const a = df.GROUP_BIT_AND("k", "v");
    assert(a.values[0] === 2147483648, "GROUP_BIT_AND keeps the high bit unsigned (got " + a.values[0] + ")");
    const x = df.GROUP_BIT_XOR("k", "v");
    assert(x.values[0] === 2147483648, "GROUP_BIT_XOR keeps the high bit unsigned (got " + x.values[0] + ")");
}

{
    const df = new DataFrame({ k: ["g", "g"], v: new Int32Array([-1, 0]) });
    const r = df.GROUP_BIT_OR("k", "v");
    assert(r.values[0] === 4294967295, "negative operands fold as ToUint32 (got " + r.values[0] + ")");
}

{
    const df = new DataFrame({ k: ["a", "b"], v: new Int32Array([12, 6]) });
    const r = df.GROUP_BIT_OR("k", "v");
    assert(r.values[0] === 12 && r.values[1] === 6, "small values unchanged");
}

print("test_a4_group_bit_sign: " + (n - fails) + " passed, " + fails + " failed");
if (fails)
    throw new Error("test_a4_group_bit_sign: " + fails + " failures");
