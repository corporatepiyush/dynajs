// covers: integration c:src/dyna-async.c -- the --timeout-ms interrupt stays uncatchable through dyna:async
// The interrupt that --timeout-ms raises is uncatchable. retry, withTimeout,
// Semaphore.run, pmap and Pool.submit used to turn it into a rejection of
// their result promise (and retry into another attempt), so user code saw it
// ("LEAK" lines). Each case runs in its own child process.
import { args, Exec } from "dyna:sys";
import { suite } from "../lib/kit.js";

const BIN = args()[0];
const DEADLINE_MS = 200;
const LIMIT_MS = 3000;
const t = suite("async_interrupt");

const pre = "function spin(){ for(;;){} }\nconst leak = (e) => console.log(\"LEAK\", e);\nsetTimeout(() => console.log(\"LEAK timer\"), 100);\n";
const da = (body) => `import("dyna:async").then(({ retry, withTimeout, Semaphore, pmap, Pool, debounce, throttle }) => { ${body} });`;
const thenSpin = "({ get then() { spin(); } })";

const CASES = [
    ["retry fn", da(`retry(() => spin(), { backoffMs: 0 }).catch(leak);`)],
    ["retry fn returns a then getter", da(`retry(() => ${thenSpin}, { backoffMs: 0 }).catch(leak);`)],
    ["retry onRetry", da(`retry(() => { throw 1; }, { retries: 2, backoffMs: 0, onRetry() { spin(); } }).catch(leak);`)],
    ["retry second attempt", da(`let n = 0; retry(() => { if (n++) spin(); throw 1; }, { retries: 3, backoffMs: 0 }).catch(leak);`)],
    ["withTimeout thenable then", da(`withTimeout({ then() { spin(); } }, 1000).catch(leak); console.log("LEAK after");`)],
    ["Semaphore.run fn", da(`new Semaphore(1).run(() => spin()).catch(leak);`)],
    ["Semaphore.run fn returns a then getter", da(`new Semaphore(1).run(() => ${thenSpin}).catch(leak);`)],
    ["pmap fn", da(`pmap([1, 2], () => spin()).catch(leak);`)],
    ["pmap fn returns a then getter", da(`pmap([1], () => ${thenSpin}).catch(leak);`)],
    ["pmap element getter", da(`const a = [1]; Object.defineProperty(a, 0, { get() { spin(); } }); pmap(a, (x) => x).catch(leak);`)],
    ["Pool.submit fn", da(`const p = new Pool(1); p.submit(() => spin()).catch(leak); p.submit(() => console.log("LEAK next job"));`)],
    ["Pool.submit fn returns a then getter", da(`const p = new Pool(1); p.submit(() => ${thenSpin}).catch(leak); p.submit(() => console.log("LEAK next job"));`)],
    ["throttle", da(`try { throttle(() => spin(), 10)(); } catch (e) { leak(e); } console.log("LEAK after");`)],
    ["debounce", da(`debounce(() => spin(), 1)();`)],
];

for (const [name, body] of CASES) {
    t.test(name, (a) => {
        const t0 = Date.now();
        const r = Exec(BIN, ["--timeout-ms", String(DEADLINE_MS), "--eval", pre + body], { timeoutMs: 10000 });
        const ms = Date.now() - t0, out = String(r.stdout), err = String(r.stderr);
        a.ok(r.code !== 0, `exits non-zero (code ${r.code})`);
        a.ok(err.includes("interrupted"), `reports interrupted (stderr ${JSON.stringify(err.slice(0, 160))})`);
        a.ok(!out.includes("LEAK"), `no user code ran after the interrupt (stdout ${JSON.stringify(out.slice(0, 160))})`);
        a.ok(ms < LIMIT_MS, `exits within ${LIMIT_MS} ms (took ${ms} ms)`);
    });
}

t.test("ordinary errors in dyna:async callbacks still reject", (a) => {
    const src = `import("dyna:async").then(async ({ retry, withTimeout, Semaphore, pmap, Pool }) => {
        await retry(() => { throw 1; }, { retries: 1, backoffMs: 0 }).catch((v) => console.log("OK retry", v));
        await withTimeout({ then() { throw 2; } }, 1000).catch((v) => console.log("OK withTimeout", v));
        await new Semaphore(1).run(() => { throw 3; }).catch((v) => console.log("OK run", v));
        await pmap([1], () => { throw 4; }).catch((v) => console.log("OK pmap", v));
        await new Pool(1).submit(() => { throw 5; }).catch((v) => console.log("OK pool", v));
    });`;
    const r = Exec(BIN, ["--timeout-ms", "5000", "--eval", src], { timeoutMs: 10000 });
    const out = String(r.stdout);
    a.eq(r.code, 0, `exit code (stderr ${JSON.stringify(String(r.stderr).slice(0, 160))})`);
    for (const m of ["OK retry 1", "OK withTimeout 2", "OK run 3", "OK pmap 4", "OK pool 5"])
        a.ok(out.includes(m), `prints ${m} (stdout ${JSON.stringify(out.slice(0, 300))})`);
});

await t.run();
