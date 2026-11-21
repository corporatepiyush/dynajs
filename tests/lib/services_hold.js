// tests/lib/services_hold.js -- services.js for a C test: `dynajs --std tests/lib/services_hold.js postgres redis`
// starts (or reuses) each named server, prints ONE JSON line { postgres: conn|null, redis: port|null, why: {...} },
// then holds them until its stdin closes and stops the ones it started (rule 15). A C harness cannot keep a second
// runtime alive beside the one under test (one reactor per thread), so it runs this as a child over two pipes.
import * as std from "std";
import { ensurePostgres, ensureRedis, stopStarted } from "./services.js";

const out = { postgres: null, redis: null, why: {} };
for (const k of scriptArgs.slice(1)) {
    if (k === "postgres") { const p = await ensurePostgres(); if (p.ok) out.postgres = p.conn; else out.why.postgres = p.why || "unavailable"; }
    else if (k === "redis") { const r = await ensureRedis(); if (r.ok) out.redis = r.port; else out.why.redis = r.why || "unavailable"; }
}
std.out.puts(JSON.stringify(out) + "\n");
std.out.flush();
while (std.in.getline() !== null) {}
await stopStarted();
