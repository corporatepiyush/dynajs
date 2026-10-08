// 18 · NDJSON ETL pipeline — reads newline-delimited JSON, validates and enriches it, writes compressed output.
//
// WHAT IT SHOWS
//   - dyna:stream ndjson(): one record at a time, with a line number on every parse error
//   - dyna:schema: a compiled contract for incoming records
//   - a dead-letter file: rejected records are kept with the reason, never silently dropped
//   - back-pressure: the writer awaits each write, so a slow disk slows the reader
//
// RUN      dynajs examples/apps/18-ndjson-etl-pipeline.js [in.ndjson] [out.ndjson.zst] [rejects.ndjson]

import { ndjson, lines, fromFile, toFile, deflate, inflate } from "dyna:stream";
import { Schema } from "dyna:schema";
import { Path, makeTempDir, writeFile, readFile, removeAll } from "dyna:file";
import { SHA256Hex } from "dyna:hash";

const selfTest = scriptArgs.length < 2;
const work = selfTest ? makeTempDir("etl") : null;
const inPath = selfTest ? work.join("events.ndjson") : new Path(scriptArgs[1]);
const outPath = selfTest ? work.join("clean.ndjson.zst") : new Path(scriptArgs[2] ?? "clean.ndjson.zst");
const rejectPath = selfTest ? work.join("rejects.ndjson") : new Path(scriptArgs[3] ?? "rejects.ndjson");

if (selfTest) {
    writeFile(inPath, [
        '{"user":"ada@example.com","event":"purchase","amount":42.5,"ts":1767225600}',
        '{"user":"bob@example.com","event":"refund","amount":10,"ts":1767225660}',
        '{"user":"eve@example.com","event":"purchase","amount":-5,"ts":1767225720}',
        '{"user":"not-an-email","event":"purchase","amount":3,"ts":1767225780}',
        '{"user":"cy@example.com","event":"login","ts":1767225840}',
        '{"user":"dan@example.com","event":"purchase","amount":7.25,"ts":1767225900,"note":"gift"}',
        "",
        '{"user":"ada@example.com","event":"purchase","amount":1,"ts":1767225960}',
    ].join("\n") + "\n");
}

const contract = Schema.compile({
    type: "object",
    required: ["user", "event", "ts"],
    properties: {
        user: { type: "string", pattern: "^[^@\\s]+@[^@\\s]+$" },
        event: { enum: ["purchase", "refund", "login"] },
        amount: { type: "number", minimum: 0 },
        ts: { type: "integer", minimum: 0 },
    },
    // A purchase or refund must carry an amount; a login must not need one.
    if: { properties: { event: { enum: ["purchase", "refund"] } } },
    then: { required: ["amount"] },
});

// Transform: pseudonymize the user (the output is shared with analysts) and
// normalize the timestamp. The salt would come from configuration.
const SALT = "rotate-me";
function transform(rec) {
    return {
        user_id: SHA256Hex(SALT + rec.user.toLowerCase()).slice(0, 16),
        event: rec.event,
        amount_cents: Math.round((rec.amount ?? 0) * 100) * (rec.event === "refund" ? -1 : 1),
        at: new Date(rec.ts * 1000).toISOString(),
    };
}

const out = deflate(toFile(outPath), { codec: "zstd", level: 6 });
const rejects = toFile(rejectPath);
const enc = new TextEncoder();
const stats = { read: 0, written: 0, rejected: 0 };

for await (const rec of ndjson(fromFile(inPath))) {
    stats.read++;
    const verdict = contract.validate(rec);
    if (!verdict.valid) {
        stats.rejected++;
        await rejects.write(enc.encode(JSON.stringify({ reason: verdict.errors[0].keyword, record: rec }) + "\n"));
        continue;
    }
    await out.write(enc.encode(JSON.stringify(transform(rec)) + "\n"));
    stats.written++;
}
await out.finish();            // a compressing sink MUST be finished, or the tail is lost
await rejects.flush();
rejects.close();
console.log(`read ${stats.read}, wrote ${stats.written}, rejected ${stats.rejected}`);

// ---- self-test: read the compressed output back ------------------------------
if (selfTest) {
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
    const clean = [];
    for await (const rec of ndjson(inflate(fromFile(outPath), { codec: "zstd" }))) clean.push(rec);
    check(stats.read === 7 && clean.length === 5 && stats.rejected === 2, "5 clean, 2 rejected, blank line skipped");
    check(clean.every((r) => /^[0-9a-f]{16}$/.test(r.user_id)), "no email address reaches the output");
    check(clean[0].user_id === clean[4].user_id, "the same user maps to the same pseudonym");
    check(clean[1].amount_cents === -1000, "refunds are negative cents");
    const reasons = [];
    for await (const line of lines(fromFile(rejectPath))) reasons.push(JSON.parse(line).reason);
    check(reasons.join() === "minimum,pattern", "each reject records why: " + reasons.join());
    console.log("self-test passed");
    removeAll(work);
}
