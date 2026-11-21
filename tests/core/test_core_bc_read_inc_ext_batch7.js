// covers: c:src/serialize/bc_read.inc.c
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_bc_read_inc_ext_batch7");
let n = 0;
function assert(c, m){ n++; if(!c) throw new Error("assert: "+m); }
function eq(a,b,m){ n++; const A=JSON.stringify(a),B=JSON.stringify(b); if(A!==B) throw new Error("eq "+m+" got "+A+" want "+B); }

const nested = { a:{ b:{ c:42 } }, list:[{x:1},{x:2}] };

__kitS.section("get dotted", () => {
eq(Object.get(nested, "a.b.c"), 42, "get dotted");
});

__kitS.section("get array", () => {
eq(Object.get(nested, ["a","b","c"]), 42, "get array");
});

__kitS.section("get default", () => {
eq(Object.get(nested, "a.z", "def"), "def", "get default");
});

__kitS.section("set mutates+creates", () => {
{ const o={a:1}; Object.set(o,"x.y",5); eq(o,{a:1,x:{y:5}},"set mutates+creates"); }
});

__kitS.section("set returns obj", () => {
{ const o={a:1}; assert(Object.set(o,"a",9)===o, "set returns obj"); eq(o.a,9,"set overwrite"); }
});

__kitS.section("defaults obj wins", () => {
eq(Object.defaults({a:1}, {a:9,b:2}), {a:1,b:2}, "defaults obj wins");
});

__kitS.section("evolve fn", () => {
eq(Object.evolve({n:x=>x*2}, {n:5,s:"k"}), {n:10,s:"k"}, "evolve fn");
});

__kitS.section("evolve nested", () => {
eq(Object.evolve({a:{b:x=>x+1}}, {a:{b:1,c:2}}), {a:{b:2,c:2}}, "evolve nested");
});

__kitS.section("mapObjIndexed", () => {
eq(Object.mapObjIndexed((v,k)=>k+v, {a:1,b:2}), {a:"a1",b:"b2"}, "mapObjIndexed");
});

__kitS.section("mergeWith", () => {
eq(Object.mergeWith((a,b)=>a+b, {x:1,y:2}, {y:3,z:4}), {x:1,y:5,z:4}, "mergeWith");
});

__kitS.section("mergeWithKey", () => {
eq(Object.mergeWithKey((k,a,b)=>k+a+b, {x:1}, {x:2}), {x:"x12"}, "mergeWithKey");
});

__kitS.section("modify", () => {
eq(Object.modify("a", x=>x+10, {a:1,b:2}), {a:11,b:2}, "modify");
});

__kitS.section("modify missing -> unchanged", () => {
eq(Object.modify("z", x=>x+10, {a:1}), {a:1}, "modify missing -> unchanged");
});

__kitS.section("modifyPath", () => {
eq(Object.modifyPath(["a","b"], x=>x*10, {a:{b:5}}), {a:{b:50}}, "modifyPath");
});

__kitS.section("pickAll includes missing key", () => {
assert("z" in Object.pickAll(["a","z"], {a:1}), "pickAll includes missing key");
});

__kitS.section("pickAll missing -> undefined", () => {
eq(Object.pickAll(["a","z"], {a:1}).z, undefined, "pickAll missing -> undefined");
});

__kitS.section("project", () => {
eq(Object.project(["a"], [{a:1,b:2},{a:3,b:4}]), [{a:1},{a:3}], "project");
});

__kitS.section("propSatisfies", () => {
assert(Object.propSatisfies(x=>x>0,"a",{a:5})===true, "propSatisfies");
});

__kitS.section("pathSatisfies", () => {
assert(Object.pathSatisfies(x=>x===42,["a","b","c"],nested)===true, "pathSatisfies");
});

__kitS.section("whereAny one passes", () => {
assert(Object.whereAny({a:x=>x>9,b:x=>x>0},{a:1,b:2})===true, "whereAny one passes");
});

__kitS.section("whereAny none", () => {
assert(Object.whereAny({a:x=>x>9},{a:1})===false, "whereAny none");
});

__kitS.section("renameKeys", () => {
eq(Object.renameKeys({a:"x",b:"y"}, {a:1,b:2,c:3}), {x:1,y:2,c:3}, "renameKeys");
});

__kitS.section("propIs", () => {
assert(Object.propIs(Number,"a",{a:5})===true && Object.propIs(String,"a",{a:5})===false, "propIs");
});

__kitS.section("ES Object.is preserved", () => {
assert(Object.is(NaN,NaN)===true && Object.is(0,-0)===false, "ES Object.is preserved");
});

__kitS.section("evolve non-mutating", () => {
{ const src={a:{b:1}}; Object.evolve({a:{b:x=>x+1}}, src); eq(src,{a:{b:1}},"evolve non-mutating"); }
});

__kitS.section("until cap -> RangeError", () => {
{ let threw=false; try{ ((x)=>false).until(x=>x)(1); }catch(e){ threw=e instanceof RangeError; } assert(threw,"until cap -> RangeError"); }
});

__kitS.section("evolve non-enum", () => {
assert(Object.getOwnPropertyDescriptor(Object,"evolve").enumerable===false,"evolve non-enum");
});

__kitS.section("o non-enum", () => {
assert(Object.getOwnPropertyDescriptor(Function.prototype,"o").enumerable===false,"o non-enum");
});
__kitS.end();

console.log("test_ext_batch7.js OK — "+n+" assertions");
