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
