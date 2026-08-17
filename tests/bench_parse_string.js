/* bench_parse_string.js -- the string-literal half of the lexer.
 *
 * PARSE-ONLY: every function is declared and never called, so eval parses the
 * string literals and executes almost nothing. The first version of this bench
 * called the functions, and 94% of its samples were JS_CallInternal -- it was
 * measuring the interpreter, not the lexer. A parse benchmark that runs the
 * code it parses is not a parse benchmark.
 *
 * The SWAR fast path in js_parse_string is worth 1.16x here and nothing at all
 * on bench/b_parse_heavy.js, which has almost no strings -- both numbers
 * matter, per CLAUDE.md section 4.
 */
function gen(n) {
    let src = "let sink=0;\n";
    for (let i = 0; i < n; i++)
        src += `function f${i}(){ return "The quick brown fox jumps over the lazy dog ${i} and keeps going for a while yet"; }\n`;
    return src + "sink;";
}
const src = gen(2000);
const t0 = performance.now();
for (let r = 0; r < 200; r++) eval(src);
print(`bench_parse_string: ${(performance.now()-t0).toFixed(0)}ms srcLen=${src.length}`);
