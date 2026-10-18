let n = 0;
function assert(c,m){ n++; if(!c) throw new Error("assert: "+m); }
function eq(a,b,m){ n++; const A=JSON.stringify(a),B=JSON.stringify(b); if(A!==B) throw new Error("eq "+m+" got "+A+" want "+B); }

const pa = Lens.prop("a");
eq(pa.view({a:1,b:2}), 1, "prop view");
eq(pa.set(9,{a:1,b:2}), {a:9,b:2}, "prop set");
eq(pa.over(x=>x*10,{a:5,b:2}), {a:50,b:2}, "prop over");
eq(pa.view({b:2}), undefined, "prop view missing");
{ const o={a:1,b:2}; pa.set(9,o); pa.over(x=>x,o); eq(o,{a:1,b:2},"prop set/over immutable"); }

const li = Lens.index(1);
eq(li.view([10,20,30]), 20, "index view");
{ const r=li.set(99,[10,20,30]); assert(Array.isArray(r),"index set keeps array"); eq(r,[10,99,30],"index set"); }
eq(li.over(x=>x+1,[10,20,30]), [10,21,30], "index over");

const lp = Lens.path(["a","b","c"]);
eq(lp.view({a:{b:{c:42}}}), 42, "path view");
eq(lp.set(7,{a:{b:{c:1}}}), {a:{b:{c:7}}}, "path set");
eq(lp.over(x=>x+1,{a:{b:{c:9}}}), {a:{b:{c:10}}}, "path over");
eq(lp.view({a:{}}), undefined, "path view missing");
eq(Lens.path("x.y").view({x:{y:5}}), 5, "path dotted view");
eq(Lens.path("x.y").set(8,{x:{y:1}}), {x:{y:8}}, "path dotted set");
{ const o={a:{b:{c:1}}}; lp.set(7,o); eq(o,{a:{b:{c:1}}},"path set immutable"); assert(true); }

const lc = Lens.lens(o=>o.total, (v,o)=>({...o,total:v}));
eq(lc.view({total:100,n:1}), 100, "custom view");
eq(lc.set(50,{total:100,n:1}), {total:50,n:1}, "custom set");
eq(lc.over(x=>x*2,{total:100,n:1}), {total:200,n:1}, "custom over");

eq(Lens.view(pa,{a:7}), 7, "static view");
eq(Lens.set(pa,8,{a:1}), {a:8}, "static set");
eq(Lens.over(pa,x=>x+1,{a:1}), {a:2}, "static over");
eq(Lens.view(lp,{a:{b:{c:3}}}), 3, "static path view");

for (const [L, s, v] of [
  [pa, {a:1,b:2}, 99],
  [li, [10,20,30], 77],
  [lp, {a:{b:{c:5}}}, 88],
  [lc, {total:1,extra:"x"}, 42],
]) {
  eq(L.view(L.set(v, s)), v, "law set-get");
  eq(L.set(L.view(s), s), s, "law get-set");
  eq(L.set(v, L.set(0, s)), L.set(v, s), "law set-set");
}

eq(Object.prototype.toString.call(pa), "[object Lens]", "toStringTag");
assert(Object.getPrototypeOf(pa) === Object.getPrototypeOf(li), "shared prototype");
assert(Object.getPrototypeOf(pa) === Lens.prototype, "proto is Lens.prototype");
eq(Object.keys(pa).length, 0, "lens config is non-enumerable");
assert(typeof Lens.prop === "function" && typeof pa.view === "function", "methods present");

{ let seen=false; for (const k in globalThis) if (k==="Lens") seen=true; assert(!seen, "Lens global non-enumerable"); }
assert(typeof Lens === "function", "Lens is a constructor function");

assert(pa instanceof Lens, "prop lens instanceof Lens");
assert(li instanceof Lens, "index lens instanceof Lens");
assert(lp instanceof Lens, "path lens instanceof Lens");
assert(lc instanceof Lens, "custom lens instanceof Lens");
assert(Lens.prototype.constructor === Lens, "Lens.prototype.constructor === Lens");
assert(typeof Lens.prop === "function" && typeof Lens.view === "function", "statics on the constructor");
{
  const g = o => o.v, s = (nv, o) => ({ ...o, v: nv });
  const byNew = new Lens(g, s);
  assert(byNew instanceof Lens, "new Lens(...) instanceof Lens");
  eq(byNew.view({ v: 5 }), 5, "new Lens view");
  eq(byNew.set(9, { v: 1, k: 2 }), { v: 9, k: 2 }, "new Lens set (immutable)");
  const byCall = Lens(g, s);
  assert(byCall instanceof Lens, "Lens(...) called as function instanceof Lens");
  eq(byCall.view({ v: 7 }), 7, "Lens(...) view");
  assert({} instanceof Lens === false, "a plain object is not a Lens");
}

{
  const inner = Lens.prop("y"), outer = Lens.prop("x");
  const o = {x:{y:1}};
  eq(outer.over(xo=>inner.set(9,xo), o), {x:{y:9}}, "nested lens compose");
}

{ let threw=false; try{ Lens.view({},{a:1}); }catch(e){ threw=e instanceof TypeError; } assert(threw,"non-lens -> TypeError"); }

console.log("test_lens_ext.js OK — "+n+" assertions");
