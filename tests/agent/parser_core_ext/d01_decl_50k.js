// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["a 0-9999-25000-49999-74999"];
__EXP[1] = ["b 0-19999"];
__EXP[2] = ["c 0-5000-10000-15000-20000-25000-30000-35000-40000-45000"];
// D1: 50k var + 20k let declarations parsed in one function via eval
// (declaration-index hash scale test), sampled reads, shadow checks.
var src = ["(function(){"];
for (var i = 0; i < 50000; i++) src.push("var v" + i + "=" + i + ";");
src.push("return [v0, v9999, v25000, v49999, v49999 + v25000].join('-');})()");
__L(0, "a", eval(src.join("")));
// 20k block-scoped lets each shadowed in a nested block
var src2 = ["(function(){"];
for (var i = 0; i < 20000; i++) src2.push("let w" + i + "=" + i + ";{ let w" + i + "=" + i + "*2; }");
src2.push("return [w0, w19999].join('-');})()");
__L(1, "b", eval(src2.join("")));
// 50k lets in ONE scope + closures sampling every 5000th
var src3 = ["(function(){"];
for (var i = 0; i < 50000; i++) src3.push("let L" + i + "=" + i + ";");
src3.push("var fs=[];");
for (var i = 0; i < 50000; i += 5000) src3.push("fs.push(function(){return L" + i + ";});");
src3.push("return fs.map(function(f){return f();}).join('-');})()");
__L(2, "c", eval(src3.join("")));

summary("parser_core_ext");
