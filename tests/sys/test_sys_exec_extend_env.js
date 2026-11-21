// flags: --std
// covers: dyna:sys.Exec
// extendEnv (dogfood G11): `env` alone REPLACES the child's environment, so
// Exec("git", .., {env: {GIT_PAGER: "cat"}}) lost PATH and HOME. extendEnv:
// true lays `env` over this process's environment. Oracle: /usr/bin/env's own
// listing of what the child received, compared with std.getenviron().
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sys_exec_extend_env");
import { Exec, Spawn } from "dyna:sys";
import * as std from "std";

let n = 0, bad = 0;
function ok(c, what) { n++; if (!c) { bad++; print("FAIL: " + what); } }
function eq(a, b, what) { const x = JSON.stringify(a), y = JSON.stringify(b); ok(x === y, what + " (got " + x + ", want " + y + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const parse = (out) => Object.fromEntries(String(out).split("\n").filter((l) => l.includes("=")).map((l) => [l.slice(0, l.indexOf("=")), l.slice(l.indexOf("=") + 1)]));

std.setenv("DYN_EXT_PARENT", "from-parent");
std.setenv("DYN_EXT_OVER", "parent-value");
std.setenv("DYN_EXT_OVERLONG", "longer-key");
std.setenv("DYN_EXT_OV", "shorter-key");
const parent = std.getenviron();
ok(typeof parent.PATH === "string" && parent.PATH.length > 0, "precondition: the parent has PATH");

__kitS.section("an env key overrides the parent's value", () => {
    const kids = parse(Exec("/usr/bin/env", [], { env: { DYN_EXT_OVER: "child-value", DYN_EXT_NEW: "x=y" }, extendEnv: true }).stdout);
    eq(kids.DYN_EXT_OVER, "child-value", "an env key overrides the parent's value");
    eq(kids.DYN_EXT_NEW, "x=y", "a new key arrives, '=' in the value kept");
    eq(kids.DYN_EXT_PARENT, "from-parent", "a parent-only key is inherited");
    eq(kids.PATH, parent.PATH, "PATH survives");
    eq(kids.DYN_EXT_OVERLONG, "longer-key", "overriding DYN_EXT_OVER keeps DYN_EXT_OVERLONG (whole-key compare, not prefix)");
    eq(kids.DYN_EXT_OV, "shorter-key", "a parent key that is a prefix of an env key (DYN_EXT_OV vs DYN_EXT_OVER) is kept");
    const want = Object.assign({}, parent, { DYN_EXT_OVER: "child-value", DYN_EXT_NEW: "x=y" });
    eq(Object.keys(kids).sort(), Object.keys(want).sort(), "the child's key set is parent ∪ env, no duplicates");
    const dupes = String(Exec("/usr/bin/env", [], { env: { DYN_EXT_OVER: "c" }, extendEnv: true }).stdout).split("\n").filter((l) => l.startsWith("DYN_EXT_OVER=")).length;
    eq(dupes, 1, "an overridden key appears exactly once");
});
__kitS.section("without extendEnv, env still replaces everything (contract unchanged)", () => {
    const kids = parse(Exec("/usr/bin/env", [], { env: { ONLY: "1" } }).stdout);
    eq(Object.keys(kids), ["ONLY"], "without extendEnv, env still replaces everything (contract unchanged)");
    const kids2 = parse(Exec("/usr/bin/env", [], { env: { ONLY: "1" }, extendEnv: false }).stdout);
    eq(Object.keys(kids2), ["ONLY"], "extendEnv: false is the same as leaving it out");
    const inh = parse(Exec("/usr/bin/env", [], { extendEnv: true }).stdout);
    eq(inh.DYN_EXT_PARENT, "from-parent", "extendEnv without env inherits as usual");
});
__kitS.section("the G11 case: a PATH-resolved program runs with an overlaid env", () => {
    const r = Exec("git", ["--version"], { env: { GIT_PAGER: "cat" }, extendEnv: true });
    eq(r.code, 0, "the G11 case: a PATH-resolved program runs with an overlaid env");
});
__kitS.end();
eq(err(() => Exec("/usr/bin/true", [], { env: {}, extendEnv: "yes" })), "TypeError: Exec: extendEnv must be a boolean", "a non-boolean extendEnv is refused");
eq(err(() => new Spawn("/usr/bin/true", [], { env: {}, extendEnv: 1 })), "TypeError: Spawn: extendEnv must be a boolean", "Spawn refuses a non-boolean too");

async function spawnEnv(opts) {
    const p = new Spawn("/usr/bin/env", [], opts);
    const buf = new Uint8Array(65536), parts = [];
    for (;;) { const k = await p.stdout.read(buf); if (!k) break; parts.push(String.fromCharCode(...buf.subarray(0, k))); }
    await p.wait();
    return parse(parts.join(""));
}
(async () => {
    const kids = await spawnEnv({ env: { DYN_EXT_OVER: "spawned" }, extendEnv: true });
    eq(kids.DYN_EXT_OVER, "spawned", "Spawn: env key overrides");
    eq(kids.DYN_EXT_PARENT, "from-parent", "Spawn: parent key inherited");
    eq(kids.PATH, parent.PATH, "Spawn: PATH survives");
    const only = await spawnEnv({ env: { ONLY: "1" } });
    eq(Object.keys(only), ["ONLY"], "Spawn: replace mode unchanged");
    print("test_sys_exec_extend_env: " + n + " checks, " + bad + " failures");
    if (bad) throw new Error(bad + " failures");
})().catch((e) => { print("FAIL: " + e); std.exit(1); });
