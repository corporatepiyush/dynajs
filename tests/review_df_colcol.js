/* review_df_colcol.js -- e7 review, dataframe col-vs-col doc-truth (P50-P59).
 *
 * Doc claims (API.md ADD + delta): the binary family's second operand is a
 * number OR ANOTHER COLUMN NAME (col-vs-col) for ADD/SUB/MUL/DIV/POW;
 * RSUB/RDIV are NUMBER-ONLY and must refuse a column name; row lengths zip
 * to the shorter; NaN/Inf propagate; an empty frame zips to an empty result;
 * unknown or string-dtype operand columns throw.
 */
import { DataFrame } from "dyna:dataframe";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const eq = (got, want) => {
    const g = Array.from(got), w2 = Array.from(want);
    return g.length === w2.length && g.every((v, i) => Object.is(v, w2[i]));
};
function errOf(fn) { try { return { v: fn() }; } catch (e) { return { e }; } }

const a = new Float64Array([1, 2, 3, 4]);
const b = new Float64Array([2, 2, 2, 2]);

P50: {
    const d = new DataFrame({ a, b });
    ok(eq(d.ADD("a", "b"), [3, 4, 5, 6]) && eq(d.SUB("a", "b"), [-1, 0, 1, 2]) &&
       eq(d.MUL("a", "b"), [2, 4, 6, 8]) && eq(d.DIV("a", "b"), [0.5, 1, 1.5, 2]) &&
       eq(d.POW("b", "a"), [2, 4, 8, 16]),
       "P50 ADD/SUB/MUL/DIV/POW col-vs-col are elementwise (" +
       JSON.stringify(Array.from(d.POW("b", "a"))) + ")");
}
P51: {
    /* dtype pairs: widen-once kernels must equal f64 arithmetic */
    const d = new DataFrame({
        f: new Float64Array([1.5, -2.25, 3.75, 0.5]),
        i: new Int32Array([3, -4, 5, 6]),
        u: new Uint8Array([1, 2, 3, 255]),
        s: new Int16Array([-1, 2, -3, 4]),
    });
    ok(eq(d.ADD("f", "i"), [4.5, -6.25, 8.75, 6.5]) &&
       eq(d.SUB("i", "u"), [2, -6, 2, -249]) &&
       eq(d.MUL("u", "s"), [-1, 4, -9, 1020]) &&
       eq(d.DIV("i", "f"), [3 / 1.5, -4 / -2.25, 5 / 3.75, 6 / 0.5]) &&
       eq(d.POW("s", "u"), [Math.pow(-1, 1), Math.pow(2, 2), Math.pow(-3, 3), Math.pow(4, 255)]),
       "P51 col-vs-col across dtype pairs matches f64 arithmetic");
}
P52: {
    const d = new DataFrame({ a, b });
    const e1 = errOf(() => d.RSUB("a", "b"));
    const e2 = errOf(() => d.RDIV("a", "b"));
    ok(e1.e && e1.e.constructor.name === "TypeError" && /number/.test(e1.e.message) &&
       /SUB/.test(e1.e.message),
       "P52 RSUB(col, columnName) REFUSES (TypeError naming SUB)", e1.e && e1.e.message);
    ok(e2.e && e2.e.constructor.name === "TypeError" && /number/.test(e2.e.message) &&
       /DIV/.test(e2.e.message),
       "P52b RDIV(col, columnName) REFUSES (TypeError naming DIV)", e2.e && e2.e.message);
}
P53: {
    const d = new DataFrame({ a, b });
    ok(eq(d.RSUB("a", 10), [9, 8, 7, 6]) && eq(d.RDIV("a", 12), [12, 6, 4, 3]),
       "P53 RSUB/RDIV number-only forms still compute k-col and k/col");
}
P54: {
    /* doc: "row lengths zip to the shorter". The CONSTRUCTOR refuses unequal
     * column lengths (all columns share one length; binds re-validate), so
     * the shorter-span kernel path is defensive and unreachable via the
     * public surface -- the parenthetical documents behavior no caller can
     * observe. Pin the refusal and report. */
    const e = errOf(() => new DataFrame({ long: new Float64Array([1, 2, 3, 4, 5]),
                                          short: new Float64Array([10, 20]) }));
    ok(e.e && e.e.constructor.name === "RangeError" && /same length/.test(e.e.message),
       "P54 unequal column lengths are refused at construction (zip-to-shorter unobservable)",
       e.e ? e.e.constructor.name + ": " + e.e.message : "ACCEPTED unequal lengths");
}
P55: {
    const d = new DataFrame({
        a: new Float64Array([1, NaN, 3, Infinity, -Infinity, 0]),
        b: new Float64Array([2, 2, NaN, Infinity, 2, 0]),
    });
    ok(eq(d.ADD("a", "b"), [3, NaN, NaN, Infinity, -Infinity, 0]) &&
       eq(d.DIV("a", "b"), [0.5, NaN, NaN, NaN, -Infinity, NaN]) &&
       eq(d.MUL("a", "b"), [2, NaN, NaN, Infinity, -Infinity, 0]),
       "P55 NaN/Infinity propagate through the col-vs-col verbs");
}
P56: {
    const e = new DataFrame({ x: new Float64Array([]), y: new Float64Array([]) });
    const r = errOf(() => e.ADD("x", "y"));
    ok(!r.e && r.v.length === 0, "P56 empty frame col-col is an empty result",
       r.e ? r.e.constructor.name + ": " + r.e.message : "len " + (r.v && r.v.length));
}
P57: {
    const d = new DataFrame({ a, b, text: ["x", "y", "z", "w"] });
    const e1 = errOf(() => d.ADD("a", "nope"));
    const e2 = errOf(() => d.ADD("a", "text"));
    const e3 = errOf(() => d.ADD("text", "b"));
    const e4 = errOf(() => d.MUL("nope", "b"));
    ok(e1.e && /nope|column/i.test(e1.e.message),
       "P57 unknown operand column name throws", e1.e && e1.e.message);
    ok(e2.e, "P57b string-dtype OPERAND column throws", e2.e && e2.e.message);
    ok(e3.e, "P57c string-dtype LEFT column throws", e3.e && e3.e.message);
    ok(e4.e, "P57d unknown LEFT column throws", e4.e && e4.e.message);
}
P58: {
    const d = new DataFrame({ a, b });
    ok(eq(d.ADD("a", "a"), [2, 4, 6, 8]) && eq(d.SUB("a", "a"), [0, 0, 0, 0]),
       "P58 self-column operand (ADD(a,a), SUB(a,a)) coherent");
    const d2 = new DataFrame({ a, b, o: new Float64Array(4) });
    const r = errOf(() => d2.ADD("a", "b", { out: "o" }));
    const cols = r.e ? null : d2.TO_COLUMNS();
    ok(!r.e && cols && eq(cols.o, [3, 4, 5, 6]),
       "P58b col-vs-col through the in-place {out: \"column\"} form",
       r.e ? r.e.message : JSON.stringify(cols && Array.from(cols.o)));
}
P59: {
    /* scalar-operand column form still works (regression guard for the
     * widened code path) */
    const d = new DataFrame({ a });
    ok(eq(d.ADD("a", 1), [2, 3, 4, 5]) && eq(d.RSUB("a", 1), [0, -1, -2, -3]),
       "P59 number-operand forms unchanged");
}

print("review_df_colcol: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_df_colcol: " + fail + " failures");
