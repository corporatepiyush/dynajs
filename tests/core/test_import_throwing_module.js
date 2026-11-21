// covers: integration c:src/parser/parser.inc.c -- import() of a module whose body throws rejects only the import() promise
// The module function's own promise is internal: rejecting it must not reach the host rejection tracker (the CLI exits 1 on one).
import { suite } from "../lib/kit.js";

const t = suite("import_throwing_module");
const tracked = [];
const onReject = (e) => { tracked.push(String(e.reason)); e.preventDefault?.(); };
globalThis.addEventListener?.("unhandledrejection", onReject);

t.test("a sync module that throws rejects import() with its error", async (a) => {
    await a.rejects(import("../lib/fixture_import_throw.js"), "plain", "import()");
    await a.rejects(import("../lib/fixture_import_throw.js"), "plain", "second import() sees the cached error");
});

t.test("a sync module that throws after a TLA dependency rejects import()", async (a) => {
    await a.rejects(import("../lib/fixture_import_throw_after_tla.js"), "after 1", "import()");
    const leaf = await import("../lib/fixture_import_tla_leaf.js");
    a.eq(leaf.v, 1, "the TLA dependency still evaluated");
});

t.test("no unhandled rejection reached the host", async (a) => {
    await new Promise((r) => setTimeout(r, 0));
    a.eq(tracked.length, 0, "tracked rejections: " + tracked.join(" | "));
});

await t.run();
