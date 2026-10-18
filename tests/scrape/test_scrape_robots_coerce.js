// Robots#allows coerces its argument BEFORE it resolves the native handle:
// the coercion runs caller code, and that code may close the Robots it was
// called on. Resolving first read the freed rule table (SIGSEGV), found by the
// signature-driven input generator (tests/lib/sigfuzz.js).
import { Robots } from "dyna:scrape";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function err(f) { try { return "returned " + f(); } catch (e) { return e.constructor.name + ": " + e.message; } }

const TXT = "User-agent: *\nDisallow: /private\n" + Array.from({ length: 64 }, (_, i) => `Disallow: /p${i}/\n`).join("");
for (const how of ["close", "dispose"]) {
    const r = new Robots(TXT);
    const arg = { toString() { r[how](); return "/private/x"; } };
    const m = err(() => r.allows(arg));
    ok(/^(TypeError|Error): .*(closed|disposed)/i.test(m), `a path whose toString ${how}s the receiver is refused as closed (${m})`);
    ok(/closed|disposed/i.test(err(() => r.allows("/x"))), `and the receiver stays closed after it (${how})`);
}
{
    const r = new Robots(TXT);
    const arg = { toString() { throw new RangeError("from toString"); } };
    ok(err(() => r.allows(arg)) === "RangeError: from toString", "a throwing toString propagates its own error");
    ok(r.allows("/public") === true && r.allows("/private/x") === false, "and the receiver is still usable (control)");
    r.close();
}

print("test_scrape_robots_coerce: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
