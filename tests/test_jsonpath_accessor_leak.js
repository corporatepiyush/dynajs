// flags: --std
// JSONPath's == on two objects walks their members with JS_GetOwnProperty and
// a full descriptor. An accessor member hands back counted references to its
// getter and setter, and only the value was freed, so every comparison that
// touched a getter left two references behind. Nothing allocates (the getter
// already exists), so no memory counter moves; what fails is the runtime's
// own teardown, which aborts while those objects are still referenced. This
// suite therefore passes only if the process also exits cleanly.
import { JSONPath } from "dyna:encoding";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };

function withAccessor(tag) {
    const o = { plain: 1 };
    Object.defineProperty(o, "acc", { get() { return tag; }, set(v) { void v; }, enumerable: true, configurable: true });
    return o;
}
const doc = [];
for (let i = 0; i < 20; i++) doc.push({ a: withAccessor(i), b: withAccessor(i), c: { plain: 1, acc: 2 } });
ok(new JSONPath("$[?@.a == @.b]").all(doc).length === 20, "objects with matching accessor members compare equal");
ok(new JSONPath("$[?@.a == @.c]").all(doc).length === 0, "an accessor member never equals a data member");
for (let i = 0; i < 50; i++) new JSONPath("$[?@.a == @.b]").all(doc);

print("test_jsonpath_accessor_leak: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failed");
