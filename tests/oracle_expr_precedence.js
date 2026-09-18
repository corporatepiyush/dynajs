/* oracle_expr_precedence.js -- the binary-expression grammar, before and after
 * the precedence ladder became a precedence climb.
 *
 * js_parse_expr_binary used to parse an operand by recursing level 8 -> 0, so
 * every expression walked nine frames whether or not it contained a single
 * binary operator. Replacing that with a climb is a rewrite of the expression
 * grammar, which is the highest-risk edit in a parser: get associativity or a
 * precedence level wrong and the program still compiles and runs, and simply
 * computes something else.
 *
 * So this does not test that expressions parse. It builds random expression
 * TREES over every binary operator, evaluates them, and hashes the results --
 * with operands chosen (small integers, and a couple of strings and objects)
 * so that a mis-grouping CHANGES THE VALUE rather than hiding inside an
 * associative operation. `-`, `/`, `%`, `<<`, `>>`, `**`, `in` and `instanceof`
 * are all non-associative or non-commutative for exactly that reason.
 *
 * Run under two builds; the hash must match.
 */
function fnv(s) {
    let h = 2166136261 >>> 0;
    for (let i = 0; i < s.length; i++) {
        h ^= s.charCodeAt(i) & 0xff;
        h = Math.imul(h, 16777619) >>> 0;
        h ^= (s.charCodeAt(i) >>> 8);
        h = Math.imul(h, 16777619) >>> 0;
    }
    return h >>> 0;
}
let acc = 0, cases = 0, errs = 0;
function feed(src) {
    cases++;
    let out;
    try { out = "V" + String(eval(src)); }
    catch (e) { errs++; out = "E" + e.name; }
    acc = (Math.imul(acc, 31) + fnv(out)) >>> 0;
}

/* every binary operator the table covers, one per precedence level */
const OPS = ["*", "/", "%", "+", "-", "<<", ">>", ">>>",
             "<", ">", "<=", ">=", "==", "!=", "===", "!==",
             "&", "^", "|"];
/* operands that make mis-grouping visible: non-commutative arithmetic, and
 * values that change type under + versus the comparisons */
const OPERANDS = ["1", "2", "3", "5", "7", "11", "0", "-1", "-3", "17"];

function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }
const rnd = lcg(20260728);

/* 1. exhaustive over OPERATOR PAIRS: a OP1 b OP2 c, which is where a wrong
 *    precedence level or a wrong associativity shows up first */
for (const o1 of OPS)
    for (const o2 of OPS)
        feed("(" + "3" + " " + o1 + " " + "5" + " " + o2 + " " + "2" + ")");

/* 2. exhaustive over operator TRIPLES for the arithmetic and shift levels,
 *    where associativity is decisive */
const CORE = ["*", "/", "%", "+", "-", "<<", ">>", "&", "^", "|"];
for (const o1 of CORE)
    for (const o2 of CORE)
        for (const o3 of CORE)
            feed("(7 " + o1 + " 3 " + o2 + " 5 " + o3 + " 2)");

/* 3. random trees, depth up to 6, so long operator runs are exercised */
function randExpr(depth) {
    if (depth === 0 || rnd() < 0.25)
        return OPERANDS[(rnd() * OPERANDS.length) | 0];
    const op = OPS[(rnd() * OPS.length) | 0];
    const l = randExpr(depth - 1), r = randExpr(depth - 1);
    return (rnd() < 0.15 ? "(" + l + ")" : l) + " " + op + " " +
           (rnd() < 0.15 ? "(" + r + ")" : r);
}
for (let i = 0; i < 4000; i++)
    feed("(" + randExpr(4) + ")");
for (let i = 0; i < 1500; i++)
    feed("(" + randExpr(6) + ")");

/* 4. the operators that are NOT in the table and must still compose correctly
 *    with the ones that are: && || ?? ?: , and the right-associative ** */
for (let i = 0; i < 1200; i++) {
    const e1 = randExpr(3), e2 = randExpr(3), e3 = randExpr(2);
    feed("(" + e1 + " && " + e2 + ")");
    feed("(" + e1 + " || " + e2 + ")");
    feed("((" + e1 + ") ?? (" + e2 + "))");
    feed("((" + e1 + ") ? (" + e2 + ") : (" + e3 + "))");
    feed("(" + e1 + ", " + e2 + ")");
}
/* ** is right-associative and lives in js_parse_unary, so its interaction with
 * the climbed levels is exactly what a botched conversion breaks */
for (const a of ["2", "3"])
    for (const b of ["2", "3"])
        for (const c of ["2", "3"])
            for (const op of ["*", "+", "-", "&", "|", "^", "<<"]) {
                feed("(" + a + " ** " + b + " " + op + " " + c + ")");
                feed("(" + a + " " + op + " " + b + " ** " + c + ")");
                feed("(" + a + " ** " + b + " ** " + c + ")");
            }

/* 5. `in` and `instanceof`, including the for-in head where `in` is NOT an
 *    operator -- the one table entry whose meaning depends on context */
feed("('a' in {a:1})");
feed("('a' in {a:1} === true)");
feed("(1 + 2 in {3:1})");
feed("([] instanceof Array)");
feed("([] instanceof Array === true)");
feed("(function(){ for (var k in {q:1}) return k; })()");
feed("(function(){ for (var i = 0; i < 3; i++); return i; })()");
feed("(function(){ var o = {}; for (o.p in {z:1}); return o.p; })()");
/* `#x in obj`, the special case the ladder tested at level 4 */
feed("(function(){ class C { #x = 1; static has(o){ return #x in o; } } return C.has(new C); })()");
feed("(function(){ class C { #x = 1; static f(o){ return (#x in o) === true; } } return C.f(new C); })()");
feed("(function(){ class C { #x = 1; static f(o){ return #x in o === true; } } return C.f(new C); })()");
feed("(function(){ class C { #x = 1; static f(o){ return #x in o | 0; } } return C.f(new C); })()");

/* 6. strings and objects, where + and the comparisons disagree about types --
 *    a regrouping that is invisible on integers shows up here */
for (const s1 of ['"a"', '"b"', '""'])
    for (const s2 of ['"a"', '"b"', '"1"'])
        for (const op of ["+", "<", ">", "==", "===", "!="]) {
            feed("(" + s1 + " " + op + " " + s2 + ")");
            feed("(" + s1 + " + 1 " + op + " " + s2 + ")");
            feed("(1 + " + s1 + " " + op + " " + s2 + " + 1)");
        }

/* 7. assignment and unary mixed in, since they bracket the climbed levels */
for (let i = 0; i < 400; i++) {
    const e = randExpr(3);
    feed("(function(){ var x = 1; x += " + e + "; return x; })()");
    feed("(function(){ var x = 1; return x = " + e + "; })()");
    feed("(-(" + e + "))");
    feed("(~(" + e + "))");
    feed("(!(" + e + "))");
    feed("(typeof (" + e + "))");
}

print("oracle_expr_precedence: " + cases + " cases, " + errs + " rejected, hash " +
      acc.toString(16));
