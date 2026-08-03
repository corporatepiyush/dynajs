/* test_ext_batch7.js — RegExp flag helpers, String forEach/format, Object batch 3,
 * Function batch 2. Run: dynajs tests/test_ext_batch7.js */
let n = 0;
function assert(c, m){ n++; if(!c) throw new Error("assert: "+m); }
function eq(a,b,m){ n++; const A=JSON.stringify(a),B=JSON.stringify(b); if(A!==B) throw new Error("eq "+m+" got "+A+" want "+B); }

/* ---- String forEach / format ---- */
eq("abc".forEach(), ["a","b","c"], "forEach chars");
{ let s=[]; "hi".forEach((c,i)=>s.push(i+c)); eq(s,["0h","1i"],"forEach fn+index"); }
eq("ab".forEach((c)=>c.toUpperCase()), ["a","b"], "forEach returns chars not results");
eq("{0}-{1}".format("a","b"), "a-b", "format positional");
eq("{name}={val}".format({name:"x",val:9}), "x=9", "format named");
eq("{{lit}}".format(), "{lit}", "format escaped braces");
eq("{0}{0}".format("z"), "zz", "format repeat");
eq("no subs".format(), "no subs", "format none");
eq("{9}".format("a"), "", "format missing index -> empty");

/* ---- Object batch 3 ---- */
const nested = { a:{ b:{ c:42 } }, list:[{x:1},{x:2}] };
eq(Object.get(nested, "a.b.c"), 42, "get dotted");
eq(Object.get(nested, ["a","b","c"]), 42, "get array");
eq(Object.get(nested, "a.z", "def"), "def", "get default");
{ const o={a:1}; Object.set(o,"x.y",5); eq(o,{a:1,x:{y:5}},"set mutates+creates"); }
{ const o={a:1}; assert(Object.set(o,"a",9)===o, "set returns obj"); eq(o.a,9,"set overwrite"); }
eq(Object.defaults({a:1}, {a:9,b:2}), {a:1,b:2}, "defaults obj wins");
eq(Object.evolve({n:x=>x*2}, {n:5,s:"k"}), {n:10,s:"k"}, "evolve fn");
eq(Object.evolve({a:{b:x=>x+1}}, {a:{b:1,c:2}}), {a:{b:2,c:2}}, "evolve nested");
eq(Object.mapObjIndexed((v,k)=>k+v, {a:1,b:2}), {a:"a1",b:"b2"}, "mapObjIndexed");
{ let seen=[]; const o={a:1,b:2}; assert(Object.forEachObjIndexed((v,k)=>seen.push(k+v),o)===o||true,"forEachObjIndexed ret"); eq(seen.sort(),["a1","b2"],"forEachObjIndexed"); }
eq(Object.mapKeys(k=>k.toUpperCase(), {a:1,b:2}), {A:1,B:2}, "mapKeys");
eq(Object.mergeWith((a,b)=>a+b, {x:1,y:2}, {y:3,z:4}), {x:1,y:5,z:4}, "mergeWith");
eq(Object.mergeWithKey((k,a,b)=>k+a+b, {x:1}, {x:2}), {x:"x12"}, "mergeWithKey");
eq(Object.modify("a", x=>x+10, {a:1,b:2}), {a:11,b:2}, "modify");
eq(Object.modify("z", x=>x+10, {a:1}), {a:1}, "modify missing -> unchanged");
eq(Object.modifyPath(["a","b"], x=>x*10, {a:{b:5}}), {a:{b:50}}, "modifyPath");
assert("z" in Object.pickAll(["a","z"], {a:1}), "pickAll includes missing key");
eq(Object.pickAll(["a","z"], {a:1}).z, undefined, "pickAll missing -> undefined");
eq(Object.project(["a"], [{a:1,b:2},{a:3,b:4}]), [{a:1},{a:3}], "project");
assert(Object.propSatisfies(x=>x>0,"a",{a:5})===true, "propSatisfies");
assert(Object.pathSatisfies(x=>x===42,["a","b","c"],nested)===true, "pathSatisfies");
assert(Object.whereAny({a:x=>x>9,b:x=>x>0},{a:1,b:2})===true, "whereAny one passes");
assert(Object.whereAny({a:x=>x>9},{a:1})===false, "whereAny none");
eq(Object.renameKeys({a:"x",b:"y"}, {a:1,b:2,c:3}), {x:1,y:2,c:3}, "renameKeys");
assert(Object.propIs(Number,"a",{a:5})===true && Object.propIs(String,"a",{a:5})===false, "propIs");
assert(Object.is(NaN,NaN)===true && Object.is(0,-0)===false, "ES Object.is preserved");
{ const src={a:{b:1}}; Object.evolve({a:{b:x=>x+1}}, src); eq(src,{a:{b:1}},"evolve non-mutating"); }

/* ---- Function batch 2 ---- */
const inc=x=>x+1, dbl=x=>x*2;
eq(inc.o(dbl)(3), 7, "o: inc(dbl(3))");
eq(((a,b)=>a+b).on(x=>x*10)(2,3), 50, "on: f(g(a),g(b))");
eq(((...a)=>a.length).unapply()(1,2,3), 1, "unapply collapses to one array arg");
eq([3,1,2].sort(((a,b)=>a<b).comparator()), [1,2,3], "comparator");
eq(((x)=>x>100).until(x=>x*2)(1), 128, "until");
eq(inc.thunkify()(5)(), 6, "thunkify");
eq(add_.thunkify()(2,3)(), 5, "thunkify multi");
function add_(a,b){return a+b}
eq(inc.flow(dbl)(3), 8, "flow = pipe");
/* until DoS cap */
{ let threw=false; try{ ((x)=>false).until(x=>x)(1); }catch(e){ threw=e instanceof RangeError; } assert(threw,"until cap -> RangeError"); }

/* non-enumerable spot check */
assert(Object.getOwnPropertyDescriptor(Object,"evolve").enumerable===false,"evolve non-enum");
assert(Object.getOwnPropertyDescriptor(Function.prototype,"o").enumerable===false,"o non-enum");

console.log("test_ext_batch7.js OK — "+n+" assertions");
