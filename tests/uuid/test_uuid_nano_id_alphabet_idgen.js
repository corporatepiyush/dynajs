// covers: dyna:uuid.NanoIDAlphabet
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_uuid_nano_id_alphabet_idgen");
import { NanoID, NanoIDAlphabet, ULID, ULIDTime } from "dyna:uuid";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}

const DEFAULT_ALPHA = "useandom-26T198340PX75pxJACKVERYMINDBUSHWOLFGQZbfghjklqvwyzrict";

__kitS.section("the biased-alphabet probe really has 62 symbols", () => {
    const A62 = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    eq(A62.length, 62, "the biased-alphabet probe really has 62 symbols");
    const counts = new Map();
    const N = 3000, LEN = 64;
    for (let i = 0; i < N; i++)
        for (const ch of NanoIDAlphabet(A62, LEN)) counts.set(ch, (counts.get(ch) || 0) + 1);
    const total = N * LEN, expect = total / 62;
    let chi = 0;
    for (const a of A62) {
        const o = counts.get(a) || 0;
        chi += ((o - expect) * (o - expect)) / expect;
    }
    assert(chi < 110, "NanoIDAlphabet(62 symbols) is unbiased (chi2 " + chi.toFixed(1) + " < 110)");

    const skew = new Map();
    for (const a of A62) skew.set(a, expect);
    skew.set("0", expect * 1.3);
    skew.set("z", expect * 0.7);
    let chiBad = 0;
    for (const a of A62) {
        const o = skew.get(a);
        chiBad += ((o - expect) * (o - expect)) / expect;
    }
    assert(chiBad > 110, "fault injection: a skewed sample DOES exceed the threshold ("
           + chiBad.toFixed(1) + ")");
});

eq(NanoIDAlphabet("ab", 32).length, 32, "NanoIDAlphabet length");

__kitS.section("NanoIDAlphabet('ab') emits only a and b", () => {
    let ok = true;
    for (const ch of NanoIDAlphabet("ab", 200)) if (ch !== "a" && ch !== "b") ok = false;
    assert(ok, "NanoIDAlphabet('ab') emits only a and b");
});

__kitS.section("a two-symbol alphabet produces both symbols", () => {
    const s = NanoIDAlphabet("ab", 400);
    assert(s.indexOf("a") >= 0 && s.indexOf("b") >= 0,
           "a two-symbol alphabet produces both symbols");
});

for (const bad of ["", "x"]) {
    let threw = false;
    try { NanoIDAlphabet(bad, 5); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "NanoIDAlphabet with " + bad.length + " symbols throws RangeError");
}

__kitS.section("NanoIDAlphabet refuses a non-ASCII alphabet", () => {
    let threw = false;
    try { NanoIDAlphabet("aé", 5); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "NanoIDAlphabet refuses a non-ASCII alphabet");
});
__kitS.end();

if (fails) {
    print("test_ids: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_ids failed");
}
print("test_ids: " + n + " assertions, 0 failures");
