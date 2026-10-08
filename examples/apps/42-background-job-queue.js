// 42 · Background job queue — a bounded producer/consumer pipeline with retries, a dead-letter list and graceful drain.
//
// WHAT IT SHOWS
//   - dyna:async Channel: back-pressure, so a fast producer cannot outrun slow workers
//   - dyna:async Semaphore: a cap on a shared downstream resource, independent of worker count
//   - dyna:async retry and withTimeout per job, with a dead-letter list for jobs that never succeed
//   - shutdown that finishes accepted work instead of dropping it
//
// RUN      dynajs examples/apps/42-background-job-queue.js
//          Everything runs on one thread: this is concurrency for I/O-shaped work, not CPU parallelism.

import { Channel, Semaphore, retry, withTimeout } from "dyna:async";

class JobQueue {
    constructor(handler, { workers = 4, capacity = 16, retries = 2, timeoutMs = 500, maxConcurrentDb = 2 } = {}) {
        this.handler = handler;
        this.options = { retries, timeoutMs };
        // The channel holds at most `capacity` waiting jobs. enqueue() awaits
        // when it is full, which pushes the slowdown back onto the producer.
        this.channel = new Channel(capacity);
        // A Channel has many senders and ONE consumer, so a single dispatcher
        // reads it and hands each job to one of `workers` slots.
        this.slots = new Semaphore(workers);
        // Workers may outnumber what the database tolerates; this second
        // semaphore is the real limit on that resource.
        this.db = new Semaphore(maxConcurrentDb);
        this.stats = { done: 0, retried: 0, dead: 0, peakDb: 0, inDb: 0, peakWorkers: 0 };
        this.deadLetters = [];
        this.inFlight = new Set();
        this.dispatcher = this.#dispatch();
    }

    enqueue(job) { return this.channel.send(job); }

    async #dispatch() {
        // for await ends when the channel is closed AND drained.
        for await (const job of this.channel) {
            // Waiting for a slot here is what keeps the channel full, and a
            // full channel is what slows the producer down.
            const release = await this.slots.acquire();
            const task = this.#process(job).finally(() => { release(); this.inFlight.delete(task); });
            this.inFlight.add(task);
            this.stats.peakWorkers = Math.max(this.stats.peakWorkers, this.inFlight.size);
        }
    }

    async #process(job) {
        try {
            await retry(
                () => withTimeout(this.handler(job, { withDb: (fn) => this.#withDb(fn) }), this.options.timeoutMs, "job timed out"),
                { retries: this.options.retries, backoffMs: 5, factor: 2, onRetry: () => this.stats.retried++ });
            this.stats.done++;
        } catch (e) {
            // Out of retries: park the job with its reason instead of losing it.
            this.stats.dead++;
            this.deadLetters.push({ job, error: e.message });
        }
    }

    async #withDb(fn) {
        return this.db.run(async () => {
            this.stats.peakDb = Math.max(this.stats.peakDb, ++this.stats.inDb);
            try { return await fn(); } finally { this.stats.inDb--; }
        });
    }

    // Stop accepting, let the dispatcher hand out what is queued, wait for the
    // jobs still running, then return.
    async drain() {
        this.channel.close();
        await this.dispatcher;
        await Promise.all([...this.inFlight]);
        return this.stats;
    }
}

// ---- demo / self-test ------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
const attempts = new Map();
const processed = [];

// A handler with the usual failure modes: transient errors, a poison message
// and a call that hangs.
async function sendEmail(job, { withDb }) {
    const n = (attempts.get(job.id) ?? 0) + 1;
    attempts.set(job.id, n);
    if (job.kind === "poison") throw new Error("malformed address");
    if (job.kind === "hang") await sleep(300);                // far longer than the 40 ms timeout
    if (job.kind === "flaky" && n < 3) throw new Error("smtp 451, try again");
    await withDb(async () => { await sleep(3); });           // record the delivery
    processed.push(job.id);
}

const queue = new JobQueue(sendEmail, { workers: 6, capacity: 4, retries: 2, timeoutMs: 40, maxConcurrentDb: 2 });

// The producer is much faster than the workers; watch enqueue() wait.
let blockedMs = 0;
const jobs = Array.from({ length: 40 }, (_, i) => ({ id: i, kind: i === 7 ? "poison" : i === 13 ? "hang" : i % 10 === 3 ? "flaky" : "ok" }));
for (const job of jobs) {
    const t = Date.now();
    await queue.enqueue(job);
    blockedMs += Date.now() - t;
}
const stats = await queue.drain();

console.log(JSON.stringify(stats));
console.log("dead letters:", queue.deadLetters.map((d) => `#${d.job.id} ${d.error}`).join("; "));

check(stats.done === 38 && stats.dead === 2, `38 delivered, 2 dead-lettered (${stats.done}/${stats.dead})`);
check(new Set(processed).size === 38, "no job was delivered twice");
check(queue.deadLetters.map((d) => d.job.id).sort((a, b) => a - b).join() === "7,13", "the poison and the hanging job are parked");
check(queue.deadLetters.find((d) => d.job.id === 13).error === "job timed out", "a hang becomes a timeout");
check(attempts.get(7) === 3, "a failing job is tried 1 + 2 times, then given up");
check(attempts.get(3) === 3 && processed.includes(3), "a flaky job succeeds on its third attempt");
check(stats.peakDb <= 2, "six workers never exceeded the database limit of two: " + stats.peakDb);
check(stats.peakWorkers <= 6 && stats.peakWorkers > 2, "up to six jobs ran at once: " + stats.peakWorkers);
check(blockedMs > 0, "the producer was slowed by back-pressure");

// After drain, the queue refuses new work rather than silently losing it.
const late = await queue.enqueue({ id: 99, kind: "ok" }).then(() => "accepted", () => "refused");
check(late === "refused", "enqueue after drain is refused");
console.log("self-test passed");
