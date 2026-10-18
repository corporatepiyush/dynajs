import {
    Bytes,
    indexOf, lastIndexOf, count,
} from "dyna:bytes";

let n = 0;
function assert(c, msg) {
    n++;
    if (!c) throw new Error("assertion failed: " + msg + " (case " + n + ")");
}
function eq(a, b, msg) { assert(a === b, msg + " (got " + a + ", want " + b + ")"); }
function assertThrows(fn, msg, ErrType) {
    let threw = false, err = null;
    try { fn(); } catch (e) { threw = true; err = e; }
    assert(threw, "expected throw: " + msg);
    if (ErrType) assert(err instanceof ErrType, msg + " (wrong error type: " + err + ")");
}
function u8(...bytes) { return new Uint8Array(bytes); }
function str(s) { return new Bytes(s); }

const HAY = str("hello world, hello bytes");

{
    eq(HAY.indexOf(u8(104, 101)), 0, "indexOf default unchanged");
    eq(HAY.indexOf(u8(104, 101), 1), 13, "indexOf fromIndex skips first");
    eq(HAY.indexOf(u8(104, 101), 13), 13, "indexOf fromIndex == match index");
    eq(HAY.indexOf(u8(104, 101), 14), -1, "indexOf fromIndex past last match");
    eq(HAY.indexOf(108, 14), 15, "indexOf byte-value needle with fromIndex");
    eq(HAY.indexOf(u8(104, 101), -3), 0, "indexOf negative fromIndex clamps to 0");
    eq(HAY.indexOf(u8(104, 101), 1e9), -1, "indexOf huge fromIndex");
    eq(HAY.indexOf(u8(104, 101), 1e9), -1, "indexOf huge fromIndex idempotent");
    eq(HAY.indexOf(u8(104, 101), 1.9), 13, "indexOf fractional fromIndex truncates (ToInteger)");
    eq(HAY.indexOf(u8(104, 101), 24), -1, "indexOf fromIndex == length");
    eq(HAY.indexOf(u8(104, 101), 25), -1, "indexOf fromIndex > length");
    eq(HAY.indexOf(u8(250), 5), -1, "absent needle with fromIndex");
    eq(HAY.indexOf(u8()), 0, "indexOf empty needle default 0 (unchanged)");
    eq(HAY.indexOf(u8(), 7), 7, "indexOf empty needle matches at fromIndex");
    eq(HAY.indexOf(u8(), 100), 24, "indexOf empty needle clamps to length");
    eq(indexOf(HAY.array, 108, 14), 15, "free indexOf fromIndex");
    eq(indexOf(u8(1, 2, 3, 2), 2, 2), 3, "free indexOf window");

    const hits = [];
    let pos = 0;
    while ((pos = HAY.indexOf(108, pos)) !== -1) { hits.push(pos); pos += 1; }
    eq(hits.join(","), "2,3,9,15,16", "successive-match loop via fromIndex");
    const two = [];
    pos = 0;
    while ((pos = HAY.indexOf(u8(108, 108), pos)) !== -1) { two.push(pos); pos += 2; }
    eq(two.join(","), "2,15", "multi-byte successive loop advances by plen");
}

{
    eq(HAY.lastIndexOf(u8(104, 101)), 13, "lastIndexOf default unchanged");
    eq(HAY.lastIndexOf(u8(104, 101), 12), 0, "lastIndexOf window excludes later match");
    eq(HAY.lastIndexOf(u8(104, 101), 13), 13, "lastIndexOf window includes match at fromIndex");
    eq(HAY.lastIndexOf(u8(104, 101), 0), 0, "lastIndexOf window shrinks to first byte");
    eq(HAY.lastIndexOf(u8(104, 101), -1), 0, "lastIndexOf negative clamps to 0");
    eq(HAY.lastIndexOf(u8(104, 101), 1e9), 13, "lastIndexOf huge fromIndex clamps to length");
    eq(HAY.lastIndexOf(u8(115)), 23, "lastIndexOf default finds the last 's'");
    eq(HAY.lastIndexOf(u8(115), 4), -1, "lastIndexOf window without any match");
    eq(HAY.lastIndexOf(u8()), 24, "lastIndexOf empty needle default length (unchanged)");
    eq(HAY.lastIndexOf(u8(), 7), 7, "lastIndexOf empty needle matches at fromIndex");
    eq(str("hello").lastIndexOf(u8(108, 108), 1), -1, "straddling match excluded");
    eq(str("hello").lastIndexOf(u8(108, 108), 2), 2, "match starting at edge included");
    eq(lastIndexOf(u8(1, 2, 3, 2), 2, 1), 1, "free lastIndexOf window");
}

{
    eq(HAY.count(108), 5, "count default unchanged");
    eq(HAY.count(108, 3), 4, "count fromIndex 3 (first 'l' at 2 excluded)");
    eq(HAY.count(108, 14), 2, "count window tail");
    eq(HAY.count(108, 100), 0, "count past end");
    eq(HAY.count(108, -5), 5, "count negative clamps to 0");
    eq(HAY.count(u8()), 25, "count empty needle default length+1 (unchanged)");
    eq(HAY.count(u8(), 7), 18, "count empty needle counts remaining window");
    eq(HAY.count(u8(104, 101), 1), 1, "count multi-byte needle window");
    eq(count(u8(1, 2, 1, 2, 1), 1, 1), 2, "free count window");
}

{
    eq(HAY.indexOfAny(u8(44, 33)), 11, "indexOfAny default unchanged");
    eq(HAY.indexOfAny(u8(44, 33), 12), -1, "indexOfAny window without a set byte");
    eq(HAY.indexOfAny(u8(44, 33), -5), 11, "indexOfAny negative clamps");
    eq(HAY.indexOfAny(u8(111, 98), 5), 7, "indexOfAny finds second-set byte in window");
    eq(HAY.indexOfAny(u8(44, 33), 100), -1, "indexOfAny past end");
    const empty = new Bytes("");
    eq(empty.indexOfAny(u8(97)), -1, "indexOfAny on empty haystack");
}

{
    eq(HAY.startsWith(u8(104, 101)), true, "startsWith default");
    eq(HAY.startsWith(104), true, "startsWith byte-value needle");
    eq(HAY.startsWith(u8(104), 0), true, "startsWith explicit 0");
    eq(HAY.startsWith(u8(119, 111), 6), true, "startsWith at fromIndex");
    eq(HAY.startsWith(u8(119, 111), 5), false, "startsWith wrong fromIndex");
    eq(HAY.startsWith(u8(104, 101, 120)), false, "startsWith needle longer than tail");
    eq(HAY.startsWith(u8()), true, "startsWith empty needle true");
    eq(HAY.startsWith(u8(), 24), true, "startsWith empty needle at end true");
    eq(HAY.startsWith(u8(), 25), true, "startsWith empty needle past end clamps (String parity)");
    eq(HAY.startsWith(u8(120), -4), false, "startsWith negative clamps to 0");
    eq(str("ab").startsWith(u8(97, 98, 99)), false, "startsWith longer than buffer");

    eq(HAY.endsWith(115), true, "endsWith default");
    eq(HAY.endsWith(u8(101, 115)), true, "endsWith view needle");
    eq(HAY.endsWith(u8(114, 108, 100), 11), true, "endsWith end window ('rld' at 11)");
    eq(HAY.endsWith(u8(119, 111, 114, 108), 11), false, "endsWith wrong tail in window");
    eq(HAY.endsWith(u8()), true, "endsWith empty needle true");
    eq(HAY.endsWith(u8(), 0), true, "endsWith empty needle at 0 true");
    eq(HAY.endsWith(100), false, "endsWith wrong needle");
    eq(HAY.endsWith(u8(97), -1), false, "endsWith negative end clamps to 0");
    eq(HAY.endsWith(u8(104), 1), true, "endsWith end window 1");
}

{
    let side = 0;
    const evil = { valueOf() { side = 1; b.array.fill(65); return 2; } };
    const b = str("xyz");
    eq(b.indexOf(120, evil), -1, "valueOf side effect precedes the buffer resolve");
    eq(side, 1, "valueOf actually ran");
}


{
    eq(Bytes.concat(u8(1, 2), u8(3), u8(4, 5)).toString(), String.fromCharCode(1, 2, 3, 4, 5),
       "concat variadic");
    eq(Bytes.concat([u8(1, 2), u8(3)]).toString(), String.fromCharCode(1, 2, 3),
       "concat array form unchanged");
    eq(Bytes.concat([u8(1)], u8(2), u8(3)).toString(), String.fromCharCode(1, 2, 3),
       "concat mixed list + variadic");
    eq(Bytes.concat(u8(1, 2)).toString(), String.fromCharCode(1, 2),
       "concat single view (variadic form of one)");
    eq(Bytes.concat(u8()).length, 0, "concat single empty view");
    assertThrows(() => Bytes.concat(), "concat no arguments still throws (historical)", TypeError);
    eq(Bytes.concat(str("ab"), u8(99)).toString(), "abc", "concat accepts handles");
    eq(Bytes.concat(new Bytes(""), new Bytes("")).length, 0, "concat only empties");
    assertThrows(() => Bytes.concat(5), "concat junk still throws", TypeError);
    assertThrows(() => Bytes.concat("nope"), "concat string junk throws", TypeError);
    assertThrows(() => Bytes.concat([1, 2]), "concat junk elements throw", TypeError);
    assertThrows(() => Bytes.concat(5, u8(1)), "concat junk first in variadic", TypeError);
    eq(Bytes.concat([]).length, 0, "concat([]) still empty");
    const shifty = { length: 2, 0: u8(1), 1: u8(2) };
    assertThrows(() => Bytes.concat(shifty), "array-like list still refused", TypeError);
}

{
    let reads = 0;
    const shifty = new Proxy([u8(1, 2, 3)], {
        get(t, k) {
            if (k === "0") { reads++; if (reads % 2 === 0) return u8(9, 9, 9, 9, 9, 9, 9, 9); }
            return t[k];
        },
    });
    assertThrows(() => Bytes.concat(shifty), "Proxy growth refused", TypeError);
    eq(reads >= 2, true, "the getter ran on both passes");

    eq(HAY.indexOf(108, NaN), 2, "NaN fromIndex -> 0");
    eq(HAY.indexOf(108, Infinity), 2, "Infinity fromIndex -> ToInt64 -> clamps to 0");
    eq(HAY.lastIndexOf(108, Infinity), -1, "lastIndexOf Infinity -> INT64_MIN -> clamps to 0 -> no match in head");
    assertThrows(() => HAY.indexOf(108, 1n), "BigInt fromIndex throws (ToInteger)", TypeError);
    eq(HAY.indexOf(108, "7"), 9, "string fromIndex coerces");

    const dv = new DataView(new ArrayBuffer(1));
    dv.setUint8(0, 44);
    eq(HAY.indexOf(dv), 11, "DataView needle");
    eq(HAY.indexOfAny(dv), 11, "DataView set for indexOfAny");
    const base = str("0123456789");
    const midView = base.slice(3, 7);
    eq(Array.from(midView.array).join(","), "51,52,53,54", "a slice handle's array is its window");
    midView.array.set(u8(97, 98), 0);
    eq(base.toString(), "012ab56789", "a write through a slice's array aliases the owner");
    eq(HAY.count(108, 2 ** 60), 0, "huge count fromIndex");
    eq(new Bytes("").indexOf(u8()), 0, "empty haystack empty needle");
}
print("test_bytes_search_bulk: all tests passed (" + n + " assertions)");
