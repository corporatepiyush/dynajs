// covers: c:src/vm/interpreter.inc.c
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_interpreter_inc_ext_batch7");
let n = 0;
function assert(c, m){ n++; if(!c) throw new Error("assert: "+m); }
function eq(a,b,m){ n++; const A=JSON.stringify(a),B=JSON.stringify(b); if(A!==B) throw new Error("eq "+m+" got "+A+" want "+B); }

__kitS.section("format positional", () => {
eq("{0}-{1}".format("a","b"), "a-b", "format positional");
});

__kitS.section("format named", () => {
eq("{name}={val}".format({name:"x",val:9}), "x=9", "format named");
});

__kitS.section("format escaped braces", () => {
eq("{{lit}}".format(), "{lit}", "format escaped braces");
});

__kitS.section("format repeat", () => {
eq("{0}{0}".format("z"), "zz", "format repeat");
});

__kitS.section("no subs", () => {
eq("no subs".format(), "no subs", "format none");
});

__kitS.section("format missing index -> empty", () => {
eq("{9}".format("a"), "", "format missing index -> empty");
});

__kitS.section("format overflow index -> empty, no argv[-1]", () => {
eq("{4294967295}".format("x"), "", "format overflow index -> empty, no argv[-1]");
});

__kitS.section("format INT_MAX+1 index -> empty", () => {
eq("{2147483648}".format("x"), "", "format INT_MAX+1 index -> empty");
});

const inc=x=>x+1, dbl=x=>x*2;

__kitS.section("o: inc(dbl(3))", () => {
eq(inc.o(dbl)(3), 7, "o: inc(dbl(3))");
});

__kitS.section("on: f(g(a),g(b))", () => {
eq(((a,b)=>a+b).on(x=>x*10)(2,3), 50, "on: f(g(a),g(b))");
});

__kitS.section("unapply collapses to one array arg", () => {
eq(((...a)=>a.length).unapply()(1,2,3), 1, "unapply collapses to one array arg");
});

__kitS.section("until", () => {
eq(((x)=>x>100).until(x=>x*2)(1), 128, "until");
});

__kitS.section("thunkify", () => {
eq(inc.thunkify()(5)(), 6, "thunkify");
});

__kitS.section("thunkify multi", () => {
eq(add_.thunkify()(2,3)(), 5, "thunkify multi");
});

function add_(a,b){return a+b}

__kitS.section("flow = pipe", () => {
eq(inc.flow(dbl)(3), 8, "flow = pipe");
});
__kitS.end();

console.log("test_ext_batch7.js OK — "+n+" assertions");
