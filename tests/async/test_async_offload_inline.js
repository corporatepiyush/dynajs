// flags: --io-threads 0
// covers: dyna:async.offload
// Owner ask E, wave 4: offload() respects --io-threads. With --io-threads 0 there is no pool, so a marked native runs
// inline on the loop thread: the result is the same as the direct call, and the promise is already settled when
// offload() returns (Promise.race([p, resolved]) picks p; mechanism, not timing). Mutation: making the fiber submit to
// a pool even when the pool is inline-only (or keeping the thread-per-call path) leaves it pending: red.
import { suite } from "../lib/kit.js";
import { same, text, settledInline } from "../lib/offload_check.js";
import { offload } from "dyna:async";
import { gzip, gunzip } from "dyna:compress";
import { SHA256 } from "dyna:hash";

const t = suite("async.offload.inline");
const IN = text(16 << 20, 1);

t.test("results equal the direct call with no pool", async (a) => {
    const z = await same(a, "gzip", gzip, [IN]);
    await same(a, "gunzip", gunzip, [z, { maxOutputBytes: 64 << 20 }]);
    await same(a, "SHA256", SHA256, [IN]);
});

t.test("the work runs inline: the promise is already settled when offload() returns", async (a) => {
    for (const [name, fn] of [["gzip", gzip], ["SHA256", SHA256]])
        a.eq(await settledInline(fn, [IN]), true, `${name}: inline with --io-threads 0`);
});

await t.run();
