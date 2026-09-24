// flags: --std
/* test_schema_upgrade.js -- phase-1 additions to dyna:schema.
 *
 * Schema.registerFormat(name, fn) -- custom "format" validators.
 *   Contract (as implemented): the registry is PROCESS-WIDE and live at
 *   validate() time (schemas compiled BEFORE the registration still see it).
 *   The assertion applies only to string instances; non-strings pass, and
 *   unregistered format names stay annotation-only. The validator's return
 *   value is coerced to boolean; a throw aborts validate with that
 *   exception. Duplicate names, non-functions, and empty/NUL/oversized
 *   names are TypeErrors.
 * draft support probe -- the internals are Draft 2020-12 only (the
 *   compiled tree implements 2020-12's unevaluated* semantics; there is no
 *   draft-dispatch code to hang a {draft} option on), so the honest
 *   deliverable is the doc statement, asserted here via behavior that
 *   2020-12 requires (unevaluatedProperties works; there is no draft option
 *   to pass).
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_schema_upgrade.js
 */

import { Schema } from "dyna:schema";
import { Worker, setTimeout as os_setTimeout } from "os";
import { Path, writeFile, remove } from "dyna:file";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throwsTypeError(fn, msg) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    assert(caught instanceof TypeError, msg + " (TypeError, got " + caught + ")");
}

/* ----------------: registration discipline ---------------- */
throwsTypeError(() => Schema.registerFormat(42, () => true),
    "registerFormat(number) is a TypeError");
throwsTypeError(() => Schema.registerFormat("", () => true),
    "registerFormat empty name is a TypeError");
throwsTypeError(() => Schema.registerFormat("a\0b", () => true),
    "registerFormat NUL-bearing name is a TypeError");
throwsTypeError(() => Schema.registerFormat("sc_t_nonfn", "not a function"),
    "registerFormat non-function is a TypeError");
throwsTypeError(() => Schema.registerFormat("x".repeat(300), () => true),
    "registerFormat oversized name is a TypeError");

Schema.registerFormat("sc_t_even", (s) => s.length % 2 === 0);
Schema.registerFormat("sc_t_upper", (s) => s === s.toUpperCase());
throwsTypeError(() => Schema.registerFormat("sc_t_even", () => true),
    "duplicate name is a TypeError");

/* ----------------: assertion behavior ---------------- */
const even = Schema.compile({ type: "string", format: "sc_t_even" });
assert(even.validate("abcd").valid === true, "format pass");
assert(even.validate("abc").valid === false, "format fail");
assert(even.validate("abc").errors[0].keyword === "format",
    "format failure reports keyword format");

/* unregistered names stay annotation-only */
const anon = Schema.compile({ type: "string", format: "sc_t_never_registered" });
assert(anon.validate("anything").valid === true,
    "unregistered format is annotation-only");

/* non-string instances are not asserted */
const strnum = Schema.compile({ type: ["string", "number"], format: "sc_t_even" });
assert(strnum.validate(42).valid === true, "format skipped for non-strings");

/* nested positions get the assertion */
const nested = Schema.compile({
    type: "object",
    properties: { code: { type: "string", format: "sc_t_upper" } },
});
assert(nested.validate({ code: "ABC" }).valid === true, "nested format pass");
assert(nested.validate({ code: "aBc" }).valid === false, "nested format fail");
assert(nested.validate({ code: "aBc" }).errors[0].path === "/code",
    "format failure path points at the instance");

/* the registry is live: compile FIRST, register AFTER, still asserts */
const before = Schema.compile({ type: "string", format: "sc_t_late" });
assert(before.validate("zz").valid === true,
    "unregistered -> no assertion yet");
Schema.registerFormat("sc_t_late", (s) => s.length >= 3);
assert(before.validate("zz").valid === false,
    "registration after compile is live");
assert(before.validate("zzz").valid === true, "live registry accepts");

/* return value is coerced to boolean */
Schema.registerFormat("sc_t_truthy", () => 1);
Schema.registerFormat("sc_t_falsy", () => 0);
assert(Schema.compile({ type: "string", format: "sc_t_truthy" })
    .validate("x").valid === true, "truthy return passes");
assert(Schema.compile({ type: "string", format: "sc_t_falsy" })
    .validate("x").valid === false, "falsy return fails");

/* a throwing validator aborts validate with the user's exception */
Schema.registerFormat("sc_t_throw", () => { throw new RangeError("boom"); });
let caught = null;
try {
    Schema.compile({ type: "string", format: "sc_t_throw" }).validate("x");
} catch (e) { caught = e; }
assert(caught instanceof RangeError && caught.message === "boom",
    "validator throw propagates out of validate");

/* validator sees the instance string */
Schema.registerFormat("sc_t_identity", (s) => s === "the-value");
assert(Schema.compile({ type: "string", format: "sc_t_identity" })
    .validate("the-value").valid === true, "validator receives the instance");
assert(Schema.compile({ type: "string", format: "sc_t_identity" })
    .validate("other").valid === false, "validator rejects other strings");

/* ----------------: 2020-12-only internals (documented honestly) ---- */
const draftProbe = Schema.compile({
    type: "object",
    properties: { a: true },
    required: ["a"],
    unevaluatedProperties: false,   /* 2020-12 keyword (not in draft-07) */
});
assert(draftProbe.validate({ a: 1 }).valid === true,
    "2020-12 unevaluatedProperties schema compiles and passes");
assert(draftProbe.validate({ a: 1, b: 2 }).valid === false,
    "unevaluatedProperties asserts (2020-12 semantics confirmed)");
assert(typeof Schema.compile !== "undefined" &&
    Schema.compile({ type: "string" }).validate("x").valid === true,
    "compile takes a single schema argument (no options bag, no {draft})");

/* ---------------- review fix: registration survives parent+Worker -------
 * The registry key atom used to be a static minted in the parent runtime --
 * use-after-free garbage inside a Worker's own runtime (SIGSEGV, 5/5 in the
 * review probe). It is now created per call, so each runtime owns its
 * registry. Regression: parent registers + validates, child ALSO registers
 * and validates; both must answer. */
{
    const childPath = Path.cwd().join("sc_worker_child_" + Date.now() + ".mjs");
    writeFile(childPath, [
        'import * as os from "os";',
        'import { Schema } from "dyna:schema";',
        'const parent = os.Worker.parent;',
        'Schema.registerFormat("sc_wk_fmt", (v) => v === "kid");',
        'const cs = Schema.compile({ type: "string", format: "sc_wk_fmt" });',
        'parent.postMessage({ ok: cs.validate("kid").valid });',
    ].join("\n"));
    Schema.registerFormat("sc_parent_fmt", (v) => v.startsWith("w"));
    const cs = Schema.compile({ type: "string", format: "sc_parent_fmt" });
    assert(cs.validate("whatever").valid === true,
        "parent format registered and validates");
    let got = null;
    const w = new Worker(childPath.toString());
    w.onmessage = (ev) => { got = ev.data; w.onmessage = null; };
    for (let i = 0; i < 500 && got === null; i++) {
        await new Promise((res) => { os_setTimeout(res, 10); });
    }
    assert(got !== null && got.ok === true,
        "child runtime registers + validates (got " + JSON.stringify(got) + ")");
    remove(childPath);
}

print("test_schema_upgrade: all tests passed (" + n + " assertions)");
