// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = [["~a", "~a"]];
__EXP[1] = ["b 0-1000-2000-3000-4000"];
__EXP[2] = [["~c", "~c"]];
// D5: closure-heavy declarations — 10k vars in one scope, 200 closures
// capturing distinct slices of it, plus 2k closures over a 100-var scope.
var src = ["(function(){"];
for (var i = 0; i < 10000; i++) src.push("var x" + i + "=" + i + ";");
src.push("var fs=[];");
for (var i = 0; i < 10000; i += 50) src.push("fs.push(function(){ return x" + i + "; });");
src.push("return fs.map(function(f){return f();}).reduce(function(a,b){return a+b;},0);})()");
__L(0, "a", eval(src.join("")));
// closures MUTATING the captured vars (shared scope, not copies)
var src2 = ["(function(){"];
for (var i = 0; i < 100; i++) src2.push("var y" + i + "=" + i + ";");
src2.push("var set=[],get=[];");
for (var i = 0; i < 100; i += 20) {
  src2.push("set.push(function(nv){ y" + i + "=nv; });");
  src2.push("get.push(function(){ return y" + i + "; });");
}
src2.push("set.forEach(function(s,idx){ s(idx*1000); });");
src2.push("return get.map(function(g){return g();}).join('-');})()");
__L(1, "b", eval(src2.join("")));
// 2000 closures over the SAME 100-var scope, all see later mutations
var src3 = ["(function(){"];
for (var i = 0; i < 100; i++) src3.push("var z" + i + "=0;");
src3.push("var fs=[];");
for (var i = 0; i < 2000; i++) src3.push("fs.push(function(){ return z0; });");
src3.push("z0 = 42;");
src3.push("return fs.map(function(f){return f();}).every(function(v){return v===42;}) && fs.length;})()");
__L(2, "c", eval(src3.join("")));

summary("parser_core_ext");
