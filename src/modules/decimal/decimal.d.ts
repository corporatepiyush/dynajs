/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:decimal
/** dyna:decimal — exact decimal arithmetic (IEEE 754-2008 decimal128, 34 digits by default) and Money in integer minor units. */
declare module "dyna:decimal" {
    // Rounding defaults differ on purpose: round() is halfEven (IEEE), toFixed() is halfUp (invoices); pass a RoundingMode when it matters.

    /** `precision` is 1..5000 significant digits (default 34); exact ops (add/sub/mul/mod/divmod) ignore it; ln/log10/exp throw past a fixed work budget. */
    interface DecimalOptions {
        precision?: number;
        rounding?: RoundingMode | string;
    }
    /** An exact decimal value; methods return new Decimals and accept Decimal, string or number operands (strings avoid binary-float input error). */
    class Decimal {
        /** value: decimal string, JS number (through its shortest round-trip text), or another Decimal. */
        constructor(value: string | number | Decimal);
        /** Exact addition — addition/multiplication are EXACT here, so `opts` is accepted and ignored on add/sub/mul/mod (only div rounds). */
        add(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        sub(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        mul(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        /** The only arithmetic that rounds; division by zero throws. */
        div(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        /** Exact truncated remainder: the sign follows the dividend (like JS %), mod by zero throws, opts ignored. */
        mod(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        /** Integer exponentiation; exponent in -10000..10000. */
        pow(n: number, opts?: DecimalOptions): Decimal;
        /** Square root, correctly rounded half-even (`rounding` ignored); a negative value throws RangeError. */
        sqrt(opts?: DecimalOptions): Decimal;
        /** e^x, correctly rounded half-even; RangeError past Emax = 999999; results below the subnormal range are 0. */
        exp(opts?: DecimalOptions): Decimal;
        /** Natural log, correctly rounded half-even; zero or negative throws RangeError. */
        ln(opts?: DecimalOptions): Decimal;
        /** Base-10 log, correctly rounded half-even and exact for powers of ten; zero or negative throws RangeError. */
        log10(opts?: DecimalOptions): Decimal;
        /** The integer toward -Infinity. Exact; no rounding context. */
        floor(): Decimal;
        /** The integer toward +Infinity. Exact; no rounding context. */
        ceil(): Decimal;
        /** The integer toward zero. Exact; no rounding context. */
        trunc(): Decimal;
        /** Exact truncated-division pair [q, r]: q toward zero, r with the dividend's sign, a == q*b + r; division by zero throws. */
        divmod(x: Decimal | string | number, opts?: DecimalOptions): [Decimal, Decimal];
        /** Exact: an integral value in [-2^63, 2^63) as a bigint; a fractional part (no truncation) or a value outside that range throws RangeError. */
        toBigInt(): bigint;
        /** The value zero. The constants are ordinary Decimals. */
        static readonly ZERO: Decimal;
        static readonly ONE: Decimal;
        static readonly TWO: Decimal;
        static readonly TEN: Decimal;
        static readonly NEG_ONE: Decimal;
        /** The magnitude. */
        abs(): Decimal;
        /** The negation; -0 is 0. */
        neg(): Decimal;
        /** -1, 0, or 1. The short legacy form of compare. */
        cmp(x: Decimal | string | number): number;
        /** True when the values are equal (1.5 equals 1.50). */
        equals(x: Decimal | string | number): boolean;
        /** A new Decimal rounded to dp decimal places; dp in -1000..1000; default mode halfEven. */
        round(dp?: number, rounding?: RoundingMode | string): Decimal;
        /** A string with exactly dp digits after the point; default mode halfUp; a negative value that rounds to zero still prints "-0.00". */
        toFixed(dp?: number, rounding?: RoundingMode | string): string;
        /** The exact decimal text. */
        toString(): string;
        toJSON(): string;
        /** The one place a Decimal may become approximate: ToNumber over the exact text. */
        toNumber(): number;
        isZero(): boolean;
        /** -1, 0, or 1. */
        sign(): number;
        /** The number of significant digits. */
        digits(): number;
    }

    /** Integral money: an integer count of minor units plus a 3-letter currency tag. */
    class Money {
        /** `new Money(1999, "USD")` is $19.99: integer arithmetic with a unit, not float-shaped arithmetic. minorDigits defaults to 2;
         *  minorUnits and minorDigits must be integers, the code is upper-cased, and opts must be an object when given. */
        constructor(minorUnits: number, currency: string, opts?: { minorDigits?: number });
        /** Parses the human form ("-1,234.56") digit by digit, never through a double; a value FINER than the minor unit is refused, not rounded ("1.500" ok, "1.005" throws). */
        static fromString(str: string, currency: string, opts?: { minorDigits?: number }): Money;
        /** From an exact Decimal; `d * 10^minorDigits` must be an integer or it is refused, not rounded. */
        static fromDecimal(d: Decimal, currency: string, opts?: { minorDigits?: number }): Money;
        /** From a raw minor-unit count: an integer number, a bigint, or an amount string that is whole minor units ("5.0" ok, "5.5" throws). */
        static fromMinor(n: number | bigint | string, currency: string, opts?: { minorDigits?: number }): Money;
        /** Both operands must share the currency code. */
        add(x: Money): Money;
        sub(x: Money): Money;
        cmp(x: Money): number;
        equals(x: Money): boolean;
        /** Scales by an integer; a fractional multiplier throws. */
        mul(n: number): Money;
        /** Splits into shares whose sum is exactly the original. */
        allocate(shares: number[]): Money[];
        /** The decimal amount with the currency's minor digits. */
        toString(): string;
        toJSON(): string;
        /** The minor-unit integer as a Number: exact through 2^53; use toString()/toDecimal() beyond. */
        amount(): number;
        /** The 3-letter code. */
        currency(): string;
        /** "$19.99" for major currencies, "19.99 USD" otherwise. */
        format(): string;
        /** The amount as an exact Decimal. */
        toDecimal(): Decimal;
    }
}
