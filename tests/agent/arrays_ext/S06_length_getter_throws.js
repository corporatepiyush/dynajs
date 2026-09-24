// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["throwlen => spread=[7,8,9] forof=[7,8,9] slice=RangeError concat=RangeError from=[7,8,9]"];
__EXP[1] = ["shortlen => spread=[1,2,3] slice=[1,2]"];
__EXP[2] = ["proxy-lie => spread=[1,2] slice=[1,2]"];
__EXP[3] = ["second-throw => spread=ok [1,2]"];
__EXP[4] = ["sub => [7,8] ctor=Array"];
// S06: length-getter lies and throws — spread/for-of follow the ITERATOR (length ignored);
// length-driven paths (slice/concat via generics) observe the getter.
// NOTE: a real Array's "length" is non-configurable and can never be an accessor, so these
// probes use plain objects with hand-written iterators (spread/for-of) and generic calls.
"use strict";
const out = typeof console !== "undefined" ? console.log : print;

// source whose length getter THROWS but whose iterator is well-behaved
function makeThrowing() {
  return {
    0: 7, 1: 8, 2: 9,
    get length() { throw new RangeError("LEN"); },
    [Symbol.iterator]() {
      const self = this;
      let i = 0;
      return { next: () => (i < 3 ? { value: self[i++], done: false } : { done: true }) };
    },
  };
}
// source whose length getter LIES (returns 2) while the iterator yields 3
function makeLying() {
  return {
    0: 1, 1: 2, 2: 3,
    get length() { return 2; },
    [Symbol.iterator]() {
      const self = this;
      let i = 0;
      return { next: () => (i < 3 ? { value: self[i++], done: false } : { done: true }) };
    },
  };
}

{
  const a = makeThrowing();
  let r1, r2, r3, r4, r5;
  try { r1 = JSON.stringify([...a]); } catch (e) { r1 = e.name; }
  try { const s = []; for (const v of a) s.push(v); r2 = JSON.stringify(s); } catch (e) { r2 = e.name; }
  try { r3 = JSON.stringify(Array.prototype.slice.call(a)); } catch (e) { r3 = e.name; }
  try { r4 = JSON.stringify([0].concat(a)); } catch (e) { r4 = e.name; }
  try { r5 = JSON.stringify(Array.from(a)); } catch (e) { r5 = e.name; }
  __L(0, "throwlen => spread=" + r1 + " forof=" + r2 + " slice=" + r3 + " concat=" + r4 + " from=" + r5);
}
{
  const b = makeLying();
  let r1, r3;
  try { r1 = JSON.stringify([...b]); } catch (e) { r1 = e.name; }
  try { r3 = JSON.stringify(Array.prototype.slice.call(b)); } catch (e) { r3 = e.name; }
  __L(1, "shortlen => spread=" + r1 + " slice=" + r3);
}
{
  // REAL array with a lying wrapper: length is fixed (non-accessor) but a Proxy can lie
  const target = [1, 2, 3, 4, 5];
  const p = new Proxy(target, {
    get(t, key, rx) {
      if (key === "length") return 2; // lie about length
      return Reflect.get(t, key, rx);
    },
  });
  let r1, r3;
  try { r1 = JSON.stringify([...p]); } catch (e) { r1 = e.name; }
  try { r3 = JSON.stringify(p.slice()); } catch (e) { r3 = e.name; }
  __L(2, "proxy-lie => spread=" + r1 + " slice=" + r3);
}
{
  // length getter throwing on the SECOND call (fast path may read length twice)
  let n = 0;
  const d = {
    0: 1, 1: 2,
    get length() { if (++n >= 2) throw new URIError("L2"); return 2; },
    [Symbol.iterator]() {
      const self = this;
      let i = 0;
      return { next: () => (i < 2 ? { value: self[i++], done: false } : { done: true }) };
    },
  };
  let r1;
  try { r1 = "ok " + JSON.stringify([...d]); } catch (e) { r1 = e.name + " after=" + (n - 1); }
  __L(3, "second-throw => spread=" + r1);
}
{
  // subclass instance: [...sub] must produce a plain Array with ALL elements
  class Sub extends Array {}
  const c = Sub.from([7, 8]);
  let r1 = JSON.stringify([...c]) + " ctor=" + [...c].constructor.name;
  __L(4, "sub => " + r1);
}

summary("arrays_ext");
