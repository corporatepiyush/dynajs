import { Schema } from "dyna:schema";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };

const strict = { type: "object", required: ["a"], properties: { a: { type: "integer" } } };
const permissive = {};

const first = Schema.validate(strict, { a: 1 });
ok(first.valid === true, "the strict schema accepts a valid instance", JSON.stringify(first));
ok(Schema.validate(strict, {}).valid === false, "the strict schema rejects a missing key");

const hd = Object.getOwnPropertyDescriptor(strict, "__dyna_compiled_hash");
ok(hd && typeof hd.value === "string", "validation seeds an own compiled-hash property",
   hd ? String(hd.value) : "none");
const compiled = Schema.compile(permissive);
ok(Schema.validate(compiled, { anything: true }).valid === true,
   "the permissive compiled schema accepts anything");

if (hd && typeof hd.value === "string") {
    Object.defineProperty(Object.prototype, "__dyna_compiled_hash",
        { value: hd.value, configurable: true, writable: true });
    Object.defineProperty(Object.prototype, "__dyna_compiled_schema",
        { value: compiled, configurable: true, writable: true });
    try {
        const fresh = { type: "object", required: ["a"], properties: { a: { type: "integer" } } };
        const poisoned = Schema.validate(fresh, {});
        ok(poisoned.valid === false,
           "an inherited compiled-schema pair cannot bypass a fresh Schema.validate",
           "got valid=" + poisoned.valid);
        const stillGood = Schema.validate(fresh, { a: 1 });
        ok(stillGood.valid === true, "the fresh schema still validates valid instances",
           JSON.stringify(stillGood));

        delete strict.__dyna_compiled_hash;
        delete strict.__dyna_compiled_schema;
        const again = Schema.validate(strict, {});
        ok(again.valid === false,
           "an inherited compiled-schema pair cannot bypass Schema.validate after an own-cache wipe",
           "got valid=" + again.valid);
    } finally {
        delete Object.prototype.__dyna_compiled_hash;
        delete Object.prototype.__dyna_compiled_schema;
    }
}

print("");
print("test_schema_cache_poison: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_schema_cache_poison: " + fail + " of " + (pass + fail) + " assertions FAILED");
