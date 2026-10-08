// 31 · Invoice engine — line items, discounts, per-line tax and rounding that always adds up.
//
// WHAT IT SHOWS
//   - dyna:decimal Money: integer minor units, so 0.1 + 0.2 problems cannot happen
//   - dyna:decimal Decimal: exact rates and an explicit rounding mode where money meets percentages
//   - Money.allocate: splitting an amount so the parts sum to the whole (the "lost cent" problem)
//   - rejecting malformed amounts instead of rounding them into the books
//
// RUN      dynajs examples/apps/31-invoice-engine.js

import { Money, Decimal } from "dyna:decimal";

const CURRENCY = "EUR";
const zero = () => new Money(0, CURRENCY);
const sum = (list) => list.reduce((a, b) => a.add(b), zero());

// Apply a percentage to an amount. The multiplication is exact; rounding to
// whole cents happens once, with a stated rule (half-up is the invoice convention).
function percentOf(amount, percent) {
    const exact = amount.toDecimal().mul(new Decimal(percent)).div(100);
    return Money.fromDecimal(exact.round(2, "halfUp"), CURRENCY);
}

function buildInvoice({ number, lines, discountPercent = "0", payInInstallments = 1 }) {
    const rows = lines.map((line) => {
        // Prices arrive as text from a form or an API. fromString parses digit
        // by digit and refuses "19.999": a fraction of a cent is not money.
        const unit = Money.fromString(line.unitPrice, CURRENCY);
        const net = unit.mul(line.quantity);
        return { ...line, unit, net };
    });
    const subtotal = sum(rows.map((r) => r.net));
    const discount = percentOf(subtotal, discountPercent);

    // The discount is spread across lines in proportion to their value, so each
    // line can be taxed at its own rate. allocate() guarantees the shares add
    // back up to the discount exactly, giving leftover cents to the largest shares.
    const shares = discount.amount() === 0
        ? rows.map(zero)
        : discount.allocate(rows.map((r) => r.net.amount()));
    rows.forEach((r, i) => {
        r.discount = shares[i];
        r.taxable = r.net.sub(shares[i]);
        r.tax = percentOf(r.taxable, r.taxRate);
    });

    // Tax is totalled per rate, as an invoice has to show it.
    const taxByRate = new Map();
    for (const r of rows) taxByRate.set(r.taxRate, (taxByRate.get(r.taxRate) ?? zero()).add(r.tax));
    const tax = sum([...taxByRate.values()]);
    const total = subtotal.sub(discount).add(tax);

    // Instalments: equal parts that sum to the total, first ones a cent larger if needed.
    const installments = total.allocate(Array(payInInstallments).fill(1));
    return { number, rows, subtotal, discount, taxByRate, tax, total, installments };
}

function render(inv) {
    const out = [`INVOICE ${inv.number}`];
    for (const r of inv.rows)
        out.push(`  ${String(r.quantity).padStart(3)} x ${r.description.padEnd(22)} ${r.unit.format().padStart(12)} ${r.net.format().padStart(12)}`);
    out.push(`  ${"Subtotal".padEnd(41)} ${inv.subtotal.format().padStart(12)}`);
    if (inv.discount.amount()) out.push(`  ${"Discount".padEnd(41)} ${("-" + inv.discount.format()).padStart(12)}`);
    for (const [rate, amount] of inv.taxByRate) out.push(`  ${("VAT " + rate + "%").padEnd(41)} ${amount.format().padStart(12)}`);
    out.push(`  ${"TOTAL".padEnd(41)} ${inv.total.format().padStart(12)}`);
    if (inv.installments.length > 1) out.push("  payable as: " + inv.installments.map((m) => m.format()).join(" + "));
    return out.join("\n");
}

const invoice = buildInvoice({
    number: "2026-0042",
    discountPercent: "12.5",
    payInInstallments: 3,
    lines: [
        { description: "Consulting (hours)", quantity: 7, unitPrice: "133.33", taxRate: "19" },
        { description: "Printed handbook", quantity: 3, unitPrice: "19.99", taxRate: "7" },
        { description: "Licence, annual", quantity: 1, unitPrice: "1,499.00", taxRate: "19" },
    ],
});
console.log(render(invoice));

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
check(invoice.subtotal.amount() === 7 * 13333 + 3 * 1999 + 149900, "the subtotal is exact in cents");
check(sum(invoice.rows.map((r) => r.discount)).equals(invoice.discount), "line discounts add up to the discount");
check(sum(invoice.installments).equals(invoice.total), "instalments add up to the total");
const cents = invoice.installments.map((m) => m.amount());
check(Math.max(...cents) - Math.min(...cents) <= 1, "instalments differ by at most one cent");
check(sum(invoice.rows.map((r) => r.taxable)).add(invoice.tax).equals(invoice.total), "taxable amounts plus tax equal the total");

// The classic float failure, for contrast.
check(0.1 + 0.2 !== 0.3, "binary floats cannot represent a tenth");
check(Money.fromString("0.10", CURRENCY).add(Money.fromString("0.20", CURRENCY)).equals(Money.fromString("0.30", CURRENCY)),
      "Money adds a tenth and two tenths exactly");

// Bad input is refused, never silently rounded.
for (const bad of ["19.999", "abc", "1,23.00"]) {
    let refused = false;
    try { Money.fromString(bad, CURRENCY); } catch (e) { refused = true; }
    check(refused, "refused amount: " + bad);
}
// Mixing currencies is an error, not an implicit conversion.
let mixed = false;
try { new Money(100, "EUR").add(new Money(100, "USD")); } catch (e) { mixed = true; }
check(mixed, "adding two currencies is refused");
console.log("self-test passed");
