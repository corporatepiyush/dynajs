// 02 · Webhook receiver — verifies HMAC signatures, rejects replays, and deduplicates deliveries.
//
// WHAT IT SHOWS
//   - dyna:crypto HMACHex + TimingSafeEqual: signature checks that do not leak timing
//   - dyna:structures LRU with a TTL: a bounded "already seen" set for idempotency
//   - dyna:log Logger: one JSON line per event, safe against log forging
//
// The scheme is the one most providers use: the sender computes
//   signature = HMAC-SHA256(secret, timestamp + "." + rawBody)
// and sends it with the timestamp. The receiver recomputes over the RAW body
// (never a re-serialized one) and refuses stale timestamps.
//
// RUN      dynajs examples/apps/02-webhook-receiver-hmac.js
// DEPLOY   PORT=8080 WEBHOOK_SECRET=... dynajs examples/apps/02-webhook-receiver-hmac.js

import { App } from "dyna:net";
import { HMACHex, TimingSafeEqual } from "dyna:crypto";
import { LRU } from "dyna:structures";
import { Logger } from "dyna:log";
import { getEnv } from "dyna:sys";

const PORT = Number(getEnv("PORT") ?? 0);
const SECRET = getEnv("WEBHOOK_SECRET") ?? "dev-only-secret-change-me";
const TOLERANCE_SEC = 300;                       // how old a delivery may be

// In the self-test the log goes to an array so the output stays quiet.
const lines = [];
const log = new Logger({ name: "webhooks", dest: PORT ? undefined : (l) => lines.push(l) });

// Delivery ids we have already processed. Capacity bounds memory; the TTL
// matches the replay window, after which a duplicate is refused as stale anyway.
const seen = new LRU(10000, { ttlMs: TOLERANCE_SEC * 1000 });

const sign = (timestamp, rawBody) => HMACHex("sha256", SECRET, timestamp + "." + rawBody);

function verify(req) {
    const ts = req.headers["x-webhook-timestamp"];
    const sig = req.headers["x-webhook-signature"];
    if (!ts || !sig) return "missing signature headers";
    const age = Math.abs(Date.now() / 1000 - Number(ts));
    if (!(age <= TOLERANCE_SEC)) return "timestamp outside the tolerance window";
    // Compare as bytes in constant time; a plain === would leak how many
    // leading characters matched.
    const enc = new TextEncoder();
    if (!TimingSafeEqual(enc.encode(sign(ts, req.body)), enc.encode(sig))) return "bad signature";
    return null;
}

const processed = [];                            // stands in for your real handler

const app = new App({ port: PORT });
app.post("/hooks/orders", (req) => {
    const problem = verify(req);
    if (problem) {
        log.warn("rejected", { reason: problem });
        return { status: 401, body: problem };
    }
    const event = JSON.parse(req.body);
    // Idempotency: providers retry, so the same delivery id can arrive twice.
    if (seen.has(event.id)) {
        log.info("duplicate", { id: event.id });
        return { status: 200, body: "duplicate ignored" };
    }
    seen.set(event.id, true);
    processed.push(event);
    log.info("accepted", { id: event.id, type: event.type });
    return { status: 202, body: "accepted" };
});
app.start();
console.log("webhook receiver on port", app.port);

// ---- self-test -------------------------------------------------------------
if (!PORT) {
    const url = `http://127.0.0.1:${app.port}/hooks/orders`;
    const deliver = async (body, { ts = String(Math.floor(Date.now() / 1000)), sig } = {}) => {
        const res = await fetch(url, {
            method: "POST", body,
            headers: { "x-webhook-timestamp": ts, "x-webhook-signature": sig ?? sign(ts, body) },
        });
        return res.status;
    };
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
    const body = JSON.stringify({ id: "evt_1", type: "order.paid", amount: 4200 });

    check(await deliver(body) === 202, "a correctly signed delivery is accepted");
    check(await deliver(body) === 200, "the retry is acknowledged but not reprocessed");
    check(processed.length === 1, "the handler ran exactly once");
    check(await deliver(body, { sig: "0".repeat(64) }) === 401, "a forged signature is refused");
    check(await deliver(body.replace("4200", "1"), { sig: sign("1", body) }) === 401,
          "a tampered body is refused");
    const old = String(Math.floor(Date.now() / 1000) - 3600);
    check(await deliver(body, { ts: old }) === 401, "a validly signed but stale delivery is refused");
    console.log("self-test passed:", lines.length, "log lines,", processed.length, "event processed");
    app.close();
}
