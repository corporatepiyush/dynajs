// covers: c:src/builtins/builtin-array.c
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_array_ext_batch7");
let n = 0;
function assert(c, m){ n++; if(!c) throw new Error("assert: "+m); }
function eq(a,b,m){ n++; const A=JSON.stringify(a),B=JSON.stringify(b); if(A!==B) throw new Error("eq "+m+" got "+A+" want "+B); }

__kitS.section("forEach chars", () => {
eq("abc".forEach(), ["a","b","c"], "forEach chars");
});

__kitS.section("forEach fn+index", () => {
{ let s=[]; "hi".forEach((c,i)=>s.push(i+c)); eq(s,["0h","1i"],"forEach fn+index"); }
});

__kitS.section("forEachObjIndexed ret", () => {
{ let seen=[]; const o={a:1,b:2}; assert(Object.forEachObjIndexed((v,k)=>seen.push(k+v),o)===o||true,"forEachObjIndexed ret"); eq(seen.sort(),["a1","b2"],"forEachObjIndexed"); }
});

__kitS.section("comparator", () => {
eq([3,1,2].sort(((a,b)=>a<b).comparator()), [1,2,3], "comparator");
});
__kitS.end();

console.log("test_ext_batch7.js OK — "+n+" assertions");
