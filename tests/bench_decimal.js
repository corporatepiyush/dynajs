/* bench_decimal.js -- what exactness costs.
 *
 * The honest framing is not "decimal is fast" -- a double add is one
 * instruction and this is not. It is "here is the price of the right answer",
 * with the double loop printed beside it as the (wrong but fast) floor.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/bench_decimal.js
 */
import { Decimal, Money } from "dyna:decimal";

let sink = 0;
function bench(name, reps, fn) {
    fn();
    const t0 = performance.now();
    for (let i = 0; i < reps; i++) sink += fn();
    const ms = performance.now() - t0;
    print("  " + name.padEnd(44) + (ms * 1e6 / reps).toFixed(1).padStart(9) + " ns/op");
    return ms * 1e6 / reps;
}

const a2 = new Decimal("19.99"), b2 = new Decimal("4.25");
const a18 = new Decimal("123456789.123456789"), b18 = new Decimal("987654321.987654321");
const a34 = new Decimal("1234567890123456789012345678901234");
const b34 = new Decimal("9876543210987654321098765432109876");
let big = "1.";
for (let i = 0; i < 200; i++) big += String((i % 9) + 1);
const a200 = new Decimal(big), b200 = new Decimal(big + "7");

print("arithmetic by operand size");
bench("add, 4 digits (money shape)", 20000, () => a2.add(b2).digits());
bench("add, 18 digits", 20000, () => a18.add(b18).digits());
bench("add, 34 digits (decimal128)", 20000, () => a34.add(b34).digits());
bench("add, 200 digits", 5000, () => a200.add(b200).digits());
bench("mul, 4 digits", 20000, () => a2.mul(b2).digits());
bench("mul, 18 digits", 10000, () => a18.mul(b18).digits());
bench("mul, 34 digits", 5000, () => a34.mul(b34).digits());
bench("mul, 200 digits", 500, () => a200.mul(b200).digits());
bench("div, 34 significant digits", 2000, () => a18.div(b18).digits());
bench("div, 6 significant digits", 5000, () => a18.div(b18, { precision: 6 }).digits());

print("\ntext, which is where decimal libraries spend their time");
bench("parse, 4 digits", 20000, () => new Decimal("19.99").digits());
bench("parse, 34 digits", 10000, () => new Decimal("1234567890123456789012345678901234").digits());
bench("toString, 4 digits", 20000, () => a2.toString().length);
bench("toString, 34 digits", 10000, () => a34.toString().length);
bench("toFixed(2), 4 digits", 20000, () => a2.toFixed(2).length);

print("\nTHE MONEY WORKLOAD: 10k additions of a 2-decimal amount");
{
    const items = [];
    for (let i = 0; i < 10000; i++) items.push(new Decimal(((i % 100) + 0.99).toFixed(2)));
    const dec = bench("Decimal, 10k adds                           ", 20, () => {
        let t = new Decimal("0");
        for (const x of items) t = t.add(x);
        return t.digits();
    });
    const cents = [];
    for (let i = 0; i < 10000; i++) cents.push(new Money(Math.round(((i % 100) + 0.99) * 100), "USD"));
    const mon = bench("Money, 10k adds (integer minor units)       ", 20, () => {
        let t = new Money(0, "USD");
        for (const x of cents) t = t.add(x);
        return t.amount();
    });
    const raw = [];
    for (let i = 0; i < 10000; i++) raw.push((i % 100) + 0.99);
    const dbl = bench("CONTROL: double +, which is WRONG           ", 20, () => {
        let t = 0;
        for (const x of raw) t += x;
        return t | 0;
    });
    print("  Decimal/double " + (dec / dbl).toFixed(0) + "x, Money/double "
          + (mon / dbl).toFixed(0) + "x -- the price of the right answer");
    /* And the reason: the double total is not the exact sum. */
    let t = 0;
    for (const x of raw) t += x;
    let d = new Decimal("0");
    for (const x of items) d = d.add(x);
    print("  double total " + t + " vs exact " + d.toString()
          + (String(t) === d.toString() ? "  (equal here)" : "  <- DIFFERENT"));
}

print("\nMoney.allocate");
{
    const m = new Money(100000, "USD");
    const w3 = [1, 1, 1], w100 = [];
    for (let i = 0; i < 100; i++) w100.push(1);
    bench("allocate into 3", 20000, () => m.allocate(w3).length);
    bench("allocate into 100", 2000, () => m.allocate(w100).length);
}
print("\nsink " + (sink === 0 ? "ZERO -- the loops were optimised away" : "ok"));
