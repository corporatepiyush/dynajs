// covers: c:src/builtins/builtin-string.c
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_string_ext_batch7");
let n = 0;
function assert(c, m){ n++; if(!c) throw new Error("assert: "+m); }
function eq(a,b,m){ n++; const A=JSON.stringify(a),B=JSON.stringify(b); if(A!==B) throw new Error("eq "+m+" got "+A+" want "+B); }

__kitS.section("forEach returns chars not results", () => {
eq("ab".forEach((c)=>c.toUpperCase()), ["a","b"], "forEach returns chars not results");
});

__kitS.section("mapKeys", () => {
eq(Object.mapKeys(k=>k.toUpperCase(), {a:1,b:2}), {A:1,B:2}, "mapKeys");
});
__kitS.end();

console.log("test_ext_batch7.js OK — "+n+" assertions");
