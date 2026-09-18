// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["fz 2023 1700000000000 2023-11-14T22:13:20.000Z"];
__EXP[4] = ["sub 2023 true true"];
__EXP[5] = [["~re", "~re"]];
__EXP[6] = ["fsub 2023 0"];
// E: frozen Date still works; subclass with getter override calling super (reentrancy)
const d = new Date(1700000000000);
Object.freeze(d);
__L(0, "fz", d.getFullYear(), d.getTime(), d.toISOString());
try { d.setTime(0); __A("e05_date_freeze_sub.js:fz-set", function () { assert_eq("no-throw", "no-throw", "fz-set"); }); } catch (e) { __L(2, "fz-set", e.constructor.name); }
__A("e05_date_freeze_sub.js:fz-after", function () { assert_eq(d.getTime(), 0, "fz-after"); });
class D extends Date {
  getFullYear() { const y = super.getFullYear(); return y; }
}
const s = new D(1700000000000);
__L(4, "sub", s.getFullYear(), s instanceof Date, s instanceof D);
class R extends Date {
  getFullYear() {
    if (!this._touched) { this._touched = true; this.setTime(1600000000000); }
    return super.getFullYear();
  }
}
const r = new R(1700000000000);
__L(5, "re", r.getFullYear(), r.getTime());
class F extends Date {}
const fz = Object.freeze(new F(1700000000000));
__L(6, "fsub", fz.getFullYear(), fz.getMilliseconds());
// sealed + preventExtended same class of behavior
const sd = Object.seal(new Date(1700000000000));
try { sd.setTime(1); __A("e05_date_freeze_sub.js:sealed", function () { assert_eq(sd.getTime(), 1, "sealed"); }); } catch (e) { __L(8, "sealed-err", e.constructor.name); }
// subclass passing this to super before ready (reentrancy through constructor)
let leaked;
class L extends Date {
  constructor(t) { super(t); }
}
class M extends L {
  constructor(t) {
    super(t);
    leaked = this.getTime();
  }
}
new M(1234567890);
__A("e05_date_freeze_sub.js:m", function () { assert_eq(leaked, 1234567890, "m"); });

summary("bbreview");
