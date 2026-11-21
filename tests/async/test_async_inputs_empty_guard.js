// flags: --std
// covers: contract dyna:async.* -- selftest of tests/lib/sigfuzz.js: an empty input sweep must fail, not pass
// A "*/" inside a dynajs.d.ts doc comment ("SHA3*/SHAKE*", wave 4) ended the comment early; the parser then read the
// dyna:async declaration as nothing callable, and test_async_inputs.js printed "0 names, 0 calls" and passed.
// sigfuzz now throws "empty sweep" on a whole-module sweep with 0 names or 0 calls and on an inputSweep() suite with 0
// callable labels. Planted proof: the real d.ts with that "*/" put back into the offload() doc comment must make both
// throw; the intact d.ts must sweep 1+ names. Mutation: deleting either guard in sigfuzz.js turns its case red.
import * as std from "std";
import { suite } from "../lib/kit.js";
import { fuzzModule, sweepNames, inputSweep, useContract, scratchCleanup } from "../lib/sigfuzz.js";

const t = suite("async.inputs.empty_guard");
const text = std.loadFile("dynajs.d.ts");

function planted() {
    const at = text.indexOf('declare module "dyna:async" {');
    const fn = text.indexOf("function offload<", at);
    const doc = text.lastIndexOf("/**", fn);
    if (!(at >= 0 && doc > at && fn > doc)) throw new Error("the d.ts has no dyna:async offload() doc comment to plant into");
    return text.slice(0, doc + 3) + " Offloadable: every digest (SHA*, SHA3*/SHAKE*, MD5, BLAKE2b/BLAKE3, XXH*, Murmur3_128, CRC32/CRC32C/Adler32). " + text.slice(doc + 3);
}

async function under(contract, body) {
    useContract(contract);
    try { return await body(); } finally { useContract(text); }
}

const message = async (p) => { try { await p; } catch (e) { return e.message; } return "(resolved)"; };

t.test("whole-module sweep: a planted comment terminator throws 'empty sweep'", async (a) => {
    a.match(await under(planted(), () => message(fuzzModule("async"))), /empty sweep/, "fuzzModule rejects");
});

t.test("inputSweep suite: a planted comment terminator throws 'empty sweep' instead of declaring no cases", async (a) => {
    await under(planted(), async () => {
        a.eq((await sweepNames("async")).length, 0, "the planted contract leaves no callable labels");
        a.match(await message(inputSweep("planted_async_inputs", "async")), /empty sweep/, "inputSweep rejects");
    });
});

t.test("the intact contract sweeps names and calls", async (a) => {
    const r = await fuzzModule("async");
    a.ok(r.names > 0 && r.calls > 0, `the intact contract sweeps ${r.names} names, ${r.calls} calls`);
    a.ok((await sweepNames("async")).length > 0, "and has callable labels");
    scratchCleanup();
});

await t.run();
