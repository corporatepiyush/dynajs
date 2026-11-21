// covers: dyna:schema.Schema.compile
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_schema_schema_compile_cache_poison");
import { Schema } from "dyna:schema";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };

const strict = { type: "object", required: ["a"], properties: { a: { type: "integer" } } };
const permissive = {};

const first = Schema.validate(strict, { a: 1 });

const hd = Object.getOwnPropertyDescriptor(strict, "__dyna_compiled_hash");

__kitS.section("validation seeds an own compiled-hash property", () => {
ok(hd && typeof hd.value === "string", "validation seeds an own compiled-hash property",
   hd ? String(hd.value) : "none");
});
__kitS.end();

const compiled = Schema.compile(permissive);

print("");
print("test_schema_cache_poison: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_schema_cache_poison: " + fail + " of " + (pass + fail) + " assertions FAILED");
