// covers: c:src/runtime/class.inc.c
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_class_inc_ext_batch8");
let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(a, b, m) { assert(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function threw(fn, Ctor, m) { try { fn(); } catch (e) { assert(!Ctor || e instanceof Ctor, m + " wrong error: " + e); return; } assert(false, m + " did not throw"); }

__kitS.section("fantasy-land sequence returns the type", () => {
    class Just {
        constructor(v) { this.v = v; }
        ["fantasy-land/map"](f) { return new Just(f(this.v)); }
        ["fantasy-land/ap"](fs) { return fs instanceof Nothing ? fs : new Just(fs.v(this.v)); }
    }
    class Nothing {
        ["fantasy-land/map"]() { return this; }
        ["fantasy-land/ap"]() { return this; }
    }
    Just["fantasy-land/of"] = v => new Just(v);

    const j = [new Just(1), new Just(2), new Just(3)].sequence(Just);
    assert(j instanceof Just, "fantasy-land sequence returns the type");
    eq(j.v, [1, 2, 3], "and the values in order");
    assert([new Just(1), new Nothing(), new Just(3)].sequence(Just) instanceof Nothing,
       "one Nothing makes the whole traversal Nothing");
    eq([].sequence(Just).v, [], "an empty traversal is of([])");
    eq([1, 2, 3].traverse(Just, x => new Just(x * 2)).v, [2, 4, 6], "traverse");
    assert([1, 2, 3].traverse(Just, x => (x > 2 ? new Nothing() : new Just(x))) instanceof Nothing,
       "traverse short-circuits too");

    class Box {
        constructor(v) { this.v = v; }
        map(f) { return new Box(f(this.v)); }
        ap(x) { return new Box(this.v(x.v)); }
    }
    Box.of = v => new Box(v);
    eq([new Box(1), new Box(2)].sequence(Box).v, [1, 2], "the ap-on-functions convention");

    class Broken { map() { return this; } }
    Broken.of = () => new Broken();
    let threw = false;
    try { [new Broken()].sequence(Broken); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "a type with no ap is refused");
});
__kitS.end();

print("test_ext_batch8: all " + n + " assertions passed");
