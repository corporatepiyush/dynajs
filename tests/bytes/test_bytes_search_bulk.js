import {
    Bytes,
    indexOf, lastIndexOf, count,
    fill as freeFill,
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
    const evil = { valueOf() { side = 1; b.fill(65); return 2; } };
    const b = str("xyz");
    eq(b.indexOf(120, evil), -1, "valueOf side effect precedes the buffer resolve");
    eq(side, 1, "valueOf actually ran");
}

{
    const b = str("hello world");
    const r = b.readBytes(6, 5);
    assert(r instanceof Uint8Array, "readBytes returns Uint8Array");
    eq(r.length, 5, "readBytes length");
    eq(r.toString === undefined ? "" : String.fromCharCode(...r), "world", "readBytes window");
    r[0] = 87;
    eq(b.readUint8(6), 119, "mutating the result leaves the source");
    b.array[7] = 82;
    eq(r[1], 111, "mutating the source leaves the result");
    eq(b.readBytes(0, 0).length, 0, "readBytes zero length");
    eq(b.readBytes(11, 0).length, 0, "readBytes at end zero length");
    assertThrows(() => b.readBytes(5, 7), "readBytes past end", RangeError);
    assertThrows(() => b.readBytes(12, 0), "readBytes off > length", RangeError);
    assertThrows(() => b.readBytes(-1, 2), "readBytes negative offset", RangeError);
    eq(b.readBytes(0, 11).length, 11, "readBytes whole buffer");

    const w = Bytes.alloc(8);
    eq(w.writeBytes(2, u8(1, 2, 3)), 3, "writeBytes returns count");
    eq(Array.from(w.array).join(","), "0,0,1,2,3,0,0,0", "writeBytes wrote at offset");
    eq(w.writeBytes(0, str("ab")), 2, "writeBytes accepts a Bytes handle as src");
    eq(Array.from(w.array).join(","), "97,98,1,2,3,0,0,0", "writeBytes from handle");
    const o = str("abcdef");
    eq(o.writeBytes(0, o.array.subarray(2, 6)), 4, "overlap: src after dst");
    eq(o.toString(), "cdefef", "overlap forward copied correctly");
    const o2 = str("abcdef");
    eq(o2.writeBytes(2, o2.array.subarray(0, 4)), 4, "overlap: src before dst");
    eq(o2.toString(), "ababcd", "overlap backward copied correctly (memmove)");
    assertThrows(() => w.writeBytes(7, u8(1, 2)), "writeBytes past end", RangeError);
    assertThrows(() => w.writeBytes(9, u8(1)), "writeBytes off > length", RangeError);
    assertThrows(() => w.writeBytes(-1, u8(1)), "writeBytes negative offset", RangeError);
    assertThrows(() => w.writeBytes(0, 5), "writeBytes junk src", TypeError);
    const f = str("abc");
    f.writeBytes(0, u8(0xFF, 0xFF, 0xFF));
    eq(f.isAscii, false, "writeBytes invalidates isAscii");
}

{
    const b = str("...");
    eq(b.fill(42), b, "fill returns the handle");
    eq(b.fill(42).fill(43, 1).toString(), "*++", "fill chains");
    eq(b.array instanceof Uint8Array, true, ".array still hands out the view");
    const v = new Uint8Array(2);
    eq(freeFill(v, 7), v, "free fill still returns the view");
    eq(Array.from(v).join(","), "7,7", "free fill wrote");
    // flags: fill through the handle still invalidates
    const g = str("abc");
    g.fill(200).fill(200);
    eq(g.isAscii, false, "chainable fill still dirties the summaries");
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
    const i8 = new Int8Array([1, 2, 3]);
    const ib = Bytes.alloc(4);
    eq(ib.writeBytes(1, i8), 3, "Int8Array src accepted");
    eq(Array.from(ib.readBytes(1, 3)).join(","), "1,2,3", "Int8Array bytes copied raw");
    const dvOut = new DataView(new ArrayBuffer(3));
    eq(ib.writeBytes(0, dvOut), 3, "DataView src accepted (zero-filled)");
    const base = str("0123456789");
    const midView = base.slice(3, 7);
    eq(Array.from(midView.readBytes(0, 4)).join(","), "51,52,53,54", "readBytes on a slice handle");
    midView.writeBytes(0, u8(97, 98));
    eq(base.toString(), "012ab56789", "writeBytes through a slice aliases the owner");
    eq(HAY.count(108, 2 ** 60), 0, "huge count fromIndex");
    eq(new Bytes("").indexOf(u8()), 0, "empty haystack empty needle");
}
print("test_bytes_search_bulk: all tests passed (" + n + " assertions)");
