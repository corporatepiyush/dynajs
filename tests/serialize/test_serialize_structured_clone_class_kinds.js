// covers: dyna:serialize.structuredClone
// HTML StructuredSerializeInternal picks the branch by internal slot: [[ErrorData]] keeps name and message, boxed
// primitives ([[NumberData]], [[StringData]], [[BooleanData]], [[BigIntData]]) keep their wrapper kind and value.
// vserialize-clone.c resolves those class ids once per process from objects built with the C API (no runtime eval).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_serialize_structured_clone_class_kinds");
import { structuredClone } from "dyna:serialize";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
const tag = (v) => Object.prototype.toString.call(v);

for (const clone of [structuredClone, globalThis.structuredClone]) {
    for (const C of [Error, TypeError, RangeError, SyntaxError]) {
        const e = clone(new C("boom"));
        assert(e instanceof C && e.name === C.name && e.message === "boom", C.name + " keeps name and message");
    }
    const num = clone(new Number(3)), str = clone(new String("s")), bool = clone(new Boolean(true)), big = clone(Object(7n));
    assert(tag(num) === "[object Number]" && num.valueOf() === 3, "Number wrapper");
    assert(tag(str) === "[object String]" && str.valueOf() === "s", "String wrapper");
    assert(tag(bool) === "[object Boolean]" && bool.valueOf() === true, "Boolean wrapper");
    assert(tag(big) === "[object BigInt]" && big.valueOf() === 7n, "BigInt wrapper");
    const o = clone({ a: [1, { b: 2 }] });
    assert(Array.isArray(o.a) && o.a[1].b === 2, "plain object and array");
}
print("test_serialize_structured_clone_class_kinds: " + (n - fails) + "/" + n);
if (fails) throw new Error(fails + " failing");
