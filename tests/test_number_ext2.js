/* test_number_ext2.js — Number formatting + iteration extensions (batch 2).
 * Run: dynajs tests/test_number_ext2.js */

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(a, b, m) {
    n++;
    const A = JSON.stringify(a), B = JSON.stringify(b);
    if (A !== B) throw new Error("eq failed: " + m + " (got " + A + ", want " + B + ")");
}

/* ---- pad ---- */
eq((5).pad(3), "005", "pad 3");
eq((42).pad(5), "00042", "pad 5");
eq((-5).pad(3), "-005", "pad negative");
eq((123).pad(2), "123", "pad shorter than number");
eq((5).pad(3, true), "+005", "pad sign");
eq((255).pad(4, false, 16), "00ff", "pad base 16");
eq((5).pad(0), "5", "pad 0");

/* ---- hex ---- */
eq((255).hex(), "ff", "hex 255");
eq((255).hex(4), "00ff", "hex padded");
eq((16).hex(), "10", "hex 16");
eq((0).hex(), "0", "hex 0");

/* ---- format ---- */
eq((1234).format(), "1,234", "format thousands");
eq((1234567).format(), "1,234,567", "format millions");
eq((1234.567).format(2), "1,234.57", "format decimals");
eq((-1234).format(), "-1,234", "format negative");
eq((999).format(), "999", "format under 1000");
eq((1234).format(0, ".", ","), "1.234", "format euro separators");
eq((1000000).format(2), "1,000,000.00", "format big with decimals");

/* ---- abbr / metric / bytes ---- */
eq((1500).abbr(1), "1.5k", "abbr 1.5k");
eq((1000000).abbr(), "1m", "abbr 1m");
eq((999).abbr(), "999", "abbr under 1k");
eq((2500000000).abbr(1), "2.5b", "abbr billions");
eq((1500).metric(1), "1.5k", "metric k");
eq((1500000).metric(1), "1.5M", "metric M");
eq((1024).bytes(0), "1KB", "bytes 1KB");
eq((1536).bytes(1), "1.5KB", "bytes 1.5KB");
eq((1048576).bytes(0), "1MB", "bytes 1MB");
eq((500).bytes(0), "500B", "bytes raw");

/* ---- ordinalize ---- */
eq((1).ordinalize(), "1st", "1st");
eq((2).ordinalize(), "2nd", "2nd");
eq((3).ordinalize(), "3rd", "3rd");
eq((4).ordinalize(), "4th", "4th");
eq((11).ordinalize(), "11th", "11th");
eq((12).ordinalize(), "12th", "12th");
eq((13).ordinalize(), "13th", "13th");
eq((21).ordinalize(), "21st", "21st");
eq((22).ordinalize(), "22nd", "22nd");
eq((101).ordinalize(), "101st", "101st");
eq((111).ordinalize(), "111th", "111th");

/* ---- duration ---- */
eq((1000).duration(), "1 second", "1 second");
eq((2000).duration(), "2 seconds", "2 seconds");
eq((60000).duration(), "1 minute", "1 minute");
eq((3600000).duration(), "1 hour", "1 hour");
eq((90000000).duration(), "1 day", "1 day");
eq((500).duration(), "500 milliseconds", "ms");

/* ---- times ---- */
eq((3).times(), [0, 1, 2], "times no fn");
eq((4).times((i) => i * i), [0, 1, 4, 9], "times squares");
eq((0).times(), [], "times 0");
eq((-5).times(), [], "times negative -> empty");
{
    let calls = 0;
    (3).times(() => { calls++; });
    eq(calls, 3, "times calls fn n times");
}
eq((3).times((v, i) => i), [0, 1, 2], "times passes index");

/* ---- upto / downto ---- */
eq((1).upto(5), [1, 2, 3, 4, 5], "upto");
eq((0).upto(10, 2), [0, 2, 4, 6, 8, 10], "upto step 2");
eq((5).upto(1), [], "upto wrong direction -> empty");
eq((5).downto(1), [5, 4, 3, 2, 1], "downto");
eq((10).downto(0, 3), [10, 7, 4, 1], "downto step 3");
eq((1).downto(5), [], "downto wrong direction -> empty");
eq((1).upto(3, 1, (v) => v * 10), [10, 20, 30], "upto with fn");
eq((3).upto(3), [3], "upto single");

/* ---- Number.range (end-EXCLUSIVE, unlike upto/downto above) ---- */
eq(Number.range(0, 5), [0, 1, 2, 3, 4], "range 0..5");
eq(Number.range(1, 10, 2), [1, 3, 5, 7, 9], "range step 2");
eq(Number.range(5, 5), [], "range empty");
eq(Number.range(5, 0), [], "range wrong direction");

/* ---- SECURITY: DoS caps reject huge counts before allocating ---- */
{
    let threw = false;
    try { (1e12).times((x) => x); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "times huge count -> RangeError (no OOM)");
    threw = false;
    try { (0).upto(1e12); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "upto huge count -> RangeError");
    threw = false;
    try { Number.range(0, 1e12); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "range huge count -> RangeError");
    threw = false;
    try { (1).upto(10, 0); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "upto step 0 -> RangeError (no infinite loop)");
}

/* ---- SECURITY: reentrant {valueOf} arg cannot corrupt the result ---- */
{
    let side = 0;
    const evilStep = { valueOf() { side++; return 2; } };
    eq((0).upto(6, evilStep), [0, 2, 4, 6], "reentrant step coerced once up front");
    assert(side >= 1, "valueOf ran");
}

/* ---- works on Number wrapper objects ---- */
eq(new Number(1234).format(), "1,234", "wrapper format");
eq(new Number(3).times(), [0, 1, 2], "wrapper times");

/* ---- non-enumerable ---- */
{
    const d = Object.getOwnPropertyDescriptor(Number.prototype, "format");
    assert(d && d.enumerable === false, "format non-enumerable");
}

console.log("test_number_ext2.js OK — " + n + " assertions");
