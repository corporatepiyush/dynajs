// covers: dyna:netip.parseAddr
// covers: dyna:netip.isValid
// parse_ipv6 reads group digits through the shared dyn_hex_digit table: every
// byte is tried inside a group and the parsed bytes are checked against a JS
// oracle (parseInt of the group text), so a table entry that is wrong for any
// byte, in either case, shows up as a wrong byte or a wrong verdict.
import { parseAddr, isValid } from "dyna:netip";
import { suite } from "../lib/kit.js";

const t = suite("test_net_ipv6_hex_digits");
const HEX = "0123456789abcdefABCDEF";
const hexv = (ch) => {
    const k = "0123456789abcdef".indexOf(ch.toLowerCase());
    return ch.length === 1 && ch.charCodeAt(0) < 128 ? k : -1;
};
let seed = 99;
const rnd = (n) => {
    seed = (Math.imul(seed, 1103515245) + 12345) >>> 0;
    return (seed >>> 8) % n;
};
const group = () => {
    let s = "";
    const k = 1 + rnd(4);
    for (let i = 0; i < k; i++) s += HEX[rnd(22)];
    return s;
};
const want = (groups) => {
    const b = [];
    for (const g of groups) {
        const v = parseInt(g, 16);
        b.push(v >> 8, v & 255);
    }
    return b;
};

t.test("random full-form addresses parse to the oracle bytes", ({ eq }) => {
    let bad = 0;
    for (let i = 0; i < 3000; i++) {
        const gs = Array.from({ length: 8 }, group);
        const r = parseAddr(gs.join(":"));
        const got = Array.from(r.bytes);
        if ((r.is6 !== true || got.join() !== want(gs).join()) && bad++ < 5) eq(got.join(), want(gs).join(), gs.join(":"));
    }
    eq(bad, 0, "mismatches");
});

t.test("every byte inside a group is a digit exactly when the oracle says so", ({ eq }) => {
    let bad = 0;
    for (let c = 1; c < 256; c++) {
        const ch = String.fromCharCode(c);
        if (ch === ":" || ch === "." || ch === "%") continue;
        for (const pos of [0, 1, 3]) {
            const g = ("abc".slice(0, pos) + ch + "def".slice(0, 3 - pos)).slice(0, 4);
            const s = "1:2:3:" + g + ":5:6:7:8";
            const d = hexv(ch);
            const gotValid = isValid(s);
            if (gotValid !== (d >= 0) && bad++ < 5) eq(gotValid, d >= 0, `isValid(${JSON.stringify(s)})`);
            if (d >= 0) {
                const got = Array.from(parseAddr(s).bytes).slice(6, 8).join();
                const exp = want([g]).join();
                if (got !== exp && bad++ < 5) eq(got, exp, `group ${JSON.stringify(g)}`);
            }
        }
    }
    eq(bad, 0, "mismatches");
});

t.test("compressed and v4-tail forms keep their hex groups", ({ eq }) => {
    eq(Array.from(parseAddr("Fe80::aBcD:9").bytes).join(), [0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xab, 0xcd, 0, 9].join());
    eq(Array.from(parseAddr("::FFff:1.2.3.4").bytes).join(), [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 1, 2, 3, 4].join());
    eq(isValid("::fffg"), false, "g is not hex");
    eq(isValid("::FFFG"), false, "G is not hex");
    eq(isValid("::12345"), false, "five digits");
});

await t.run();
