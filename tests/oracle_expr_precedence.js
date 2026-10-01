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

const OPS = ["*", "/", "%", "+", "-", "<<", ">>", ">>>",
             "<", ">", "<=", ">=", "==", "!=", "===", "!==",
             "&", "^", "|"];
const OPERANDS = ["1", "2", "3", "5", "7", "11", "0", "-1", "-3", "17"];

function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }
const rnd = lcg(20260728);

for (const o1 of OPS)
    for (const o2 of OPS)
        feed("(" + "3" + " " + o1 + " " + "5" + " " + o2 + " " + "2" + ")");

const CORE = ["*", "/", "%", "+", "-", "<<", ">>", "&", "^", "|"];
for (const o1 of CORE)
    for (const o2 of CORE)
        for (const o3 of CORE)
            feed("(7 " + o1 + " 3 " + o2 + " 5 " + o3 + " 2)");

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

for (let i = 0; i < 1200; i++) {
    const e1 = randExpr(3), e2 = randExpr(3), e3 = randExpr(2);
    feed("(" + e1 + " && " + e2 + ")");
    feed("(" + e1 + " || " + e2 + ")");
    feed("((" + e1 + ") ?? (" + e2 + "))");
    feed("((" + e1 + ") ? (" + e2 + ") : (" + e3 + "))");
    feed("(" + e1 + ", " + e2 + ")");
}
for (const a of ["2", "3"])
    for (const b of ["2", "3"])
        for (const c of ["2", "3"])
            for (const op of ["*", "+", "-", "&", "|", "^", "<<"]) {
                feed("(" + a + " ** " + b + " " + op + " " + c + ")");
                feed("(" + a + " " + op + " " + b + " ** " + c + ")");
                feed("(" + a + " ** " + b + " ** " + c + ")");
            }

feed("('a' in {a:1})");
feed("('a' in {a:1} === true)");
feed("(1 + 2 in {3:1})");
feed("([] instanceof Array)");
feed("([] instanceof Array === true)");
feed("(function(){ for (var k in {q:1}) return k; })()");
feed("(function(){ for (var i = 0; i < 3; i++); return i; })()");
feed("(function(){ var o = {}; for (o.p in {z:1}); return o.p; })()");
feed("(function(){ class C { #x = 1; static has(o){ return #x in o; } } return C.has(new C); })()");
feed("(function(){ class C { #x = 1; static f(o){ return (#x in o) === true; } } return C.f(new C); })()");
feed("(function(){ class C { #x = 1; static f(o){ return #x in o === true; } } return C.f(new C); })()");
feed("(function(){ class C { #x = 1; static f(o){ return #x in o | 0; } } return C.f(new C); })()");

for (const s1 of ['"a"', '"b"', '""'])
    for (const s2 of ['"a"', '"b"', '"1"'])
        for (const op of ["+", "<", ">", "==", "===", "!="]) {
            feed("(" + s1 + " " + op + " " + s2 + ")");
            feed("(" + s1 + " + 1 " + op + " " + s2 + ")");
            feed("(1 + " + s1 + " " + op + " " + s2 + " + 1)");
        }

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
