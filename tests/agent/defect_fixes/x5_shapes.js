const out = console.log;
function counterObj(first, second) {
  let n = 0;
  const it1 = function* () { yield "A"; };
  const it2 = function* () { yield "B"; };
  return { get [Symbol.iterator]() { n++; return n === 1 ? it1 : it2; }, count: () => n };
}
{
  const o = counterObj();
  out("A1", JSON.stringify([...o]), "gets=" + o.count());
}
{
  const o = counterObj();
  const r = (function () { return [...arguments].join(""); })(...o);
  out("A2", r, "gets=" + o.count());
}
{
  const o = counterObj();
  function F(...a) { this.a = a.join(""); }
  const r = new F(...o);
  out("A3", r.a, "gets=" + o.count());
}
{
  let n = 0;
  function mk(v) {
    return { get [Symbol.iterator]() { n++; return function* () { yield v; }; } };
  }
  const a = mk("x"), b = mk("y");
  out("A4", JSON.stringify([...a, 0, ...b]), "gets=" + n);
}
try { [...5]; out("B1", "ok"); } catch (e) { out("B1", e.constructor.name); }
try { [...{}]; out("B2", "ok"); } catch (e) { out("B2", e.constructor.name); }
{
  let n = 0;
  const o = { get [Symbol.iterator]() { n++; return undefined; } };
  try { [...o]; out("B3", "ok"); } catch (e) { out("B3", e.constructor.name, "gets=" + n); }
}
{
  let n = 0;
  const o = { get [Symbol.iterator]() { n++; return function* () { yield 7; }; } };
  out("C1", Math.max(...o), "gets=" + n);
}
out("D1", JSON.stringify([..."ab"]));
{
  let runs = 0;
  function* g() { runs++; yield 1; yield 2; }
  const o = { *[Symbol.iterator]() { yield* g(); } };
  out("E1", JSON.stringify([...o]), "runs=" + runs);
}
