// promise_scheduler.js — a concurrency-limited async task scheduler with
// per-task timeouts and retry-with-backoff. Results stream out of an async
// generator as tasks settle.
//
// The scheduler takes an injected clock, so tests drive a deterministic
// *virtual* clock (no wall-clock sleeps, no flakiness). Production code would
// pass a real clock backed by setTimeout.
//
// Engine features exercised: async/await, `async function*` generators, a
// hand-rolled async channel built on Promise.withResolvers, Promise.race for
// timeouts, worker-pool concurrency, and structured result records.

import { test, run, assert, assertEqual, assertClose } from "./harness.js";

/** Raised when a task exceeds its timeout budget. */
export class TimeoutError extends Error {
  constructor(ms) {
    super(`task timed out after ${ms}ms`);
    this.name = "TimeoutError";
  }
}

/**
 * An unbounded async channel (a.k.a. async queue). Producers `push`, a single
 * consumer iterates with `for await`. Backed by Promise.withResolvers so the
 * consumer suspends cleanly when the buffer is empty.
 */
class AsyncChannel {
  #buffer = [];
  #waiter = null; // { resolve } awaiting the next value
  #closed = false;

  push(value) {
    if (this.#waiter) {
      const { resolve } = this.#waiter;
      this.#waiter = null;
      resolve({ value, done: false });
    } else {
      this.#buffer.push(value);
    }
  }

  close() {
    this.#closed = true;
    if (this.#waiter) {
      const { resolve } = this.#waiter;
      this.#waiter = null;
      resolve({ value: undefined, done: true });
    }
  }

  async *[Symbol.asyncIterator]() {
    for (;;) {
      if (this.#buffer.length > 0) {
        yield this.#buffer.shift();
        continue;
      }
      if (this.#closed) return;
      const { promise, resolve } = Promise.withResolvers();
      this.#waiter = { resolve };
      const next = await promise;
      if (next.done) return;
      yield next.value;
    }
  }
}

/**
 * A deterministic virtual clock. `sleep`/`setTimer` register timers keyed on
 * virtual time; `drain` fires them in time order, flushing microtasks between
 * each so suspended async functions make progress before the next timer.
 */
export class VirtualClock {
  #now = 0;
  #seq = 0;
  #timers = new Map(); // id -> { time, cb }

  now() { return this.#now; }

  setTimer(delayMs, cb) {
    const id = ++this.#seq;
    this.#timers.set(id, { time: this.#now + delayMs, cb });
    return id;
  }

  clearTimer(id) { this.#timers.delete(id); }

  sleep(ms) {
    const { promise, resolve } = Promise.withResolvers();
    this.setTimer(ms, resolve);
    return promise;
  }

  /**
   * Advance through all timers in virtual-time order until none remain.
   *
   * Microtasks are flushed *before* each emptiness check so that pending async
   * work (e.g. a rejection propagating into a retry's backoff `sleep`) has a
   * chance to register its next timer before we decide we are done. This
   * assumes every async delay in the system routes through this clock.
   */
  async drain() {
    for (;;) {
      await flushMicrotasks();
      if (this.#timers.size === 0) break;
      let earliestId = null;
      let earliest = null;
      for (const [id, timer] of this.#timers) {
        if (earliest === null || timer.time < earliest.time) {
          earliest = timer;
          earliestId = id;
        }
      }
      this.#timers.delete(earliestId);
      this.#now = Math.max(this.#now, earliest.time);
      earliest.cb();
    }
  }
}

/** Yield the microtask queue enough times for deep await chains to settle. */
async function flushMicrotasks(turns = 64) {
  for (let i = 0; i < turns; i++) await Promise.resolve();
}

/** Wrap a promise with a timeout that rejects via the injected clock. */
async function withTimeout(promise, ms, clock) {
  if (!Number.isFinite(ms)) return promise;
  const { promise: timeout, reject } = Promise.withResolvers();
  const timer = clock.setTimer(ms, () => reject(new TimeoutError(ms)));
  try {
    return await Promise.race([promise, timeout]);
  } finally {
    clock.clearTimer(timer);
  }
}

/**
 * @typedef {Object} Task
 * @property {string} id
 * @property {(ctx: { attempt: number, clock: VirtualClock }) => Promise<any>} run
 */

/**
 * @typedef {Object} TaskResult
 * @property {string} id
 * @property {boolean} ok
 * @property {any} [value]
 * @property {Error} [error]
 * @property {number} attempts
 */

export class Scheduler {
  #tasks = [];
  #opts;
  #clock;
  #channel = new AsyncChannel();
  #active = 0;
  #peak = 0;
  #started = false;

  constructor({ concurrency = 4, retries = 0, timeout = Infinity, backoff = () => 0, clock }) {
    if (!clock) throw new Error("Scheduler requires an injected clock");
    this.#opts = { concurrency, retries, timeout, backoff };
    this.#clock = clock;
  }

  /** Peak number of simultaneously-running tasks (for verifying the limit). */
  get peakConcurrency() { return this.#peak; }

  add(task) {
    if (this.#started) throw new Error("cannot add tasks after start()");
    this.#tasks.push(task);
    return this;
  }

  /** Async iterable of TaskResult, in completion order. */
  results() {
    return this.#channel;
  }

  /** Launch the worker pool. Returns immediately; results stream via results(). */
  start() {
    if (this.#started) throw new Error("already started");
    this.#started = true;

    const queue = this.#tasks.slice();
    let cursor = 0;
    const workerCount = Math.min(this.#opts.concurrency, Math.max(1, queue.length));
    let liveWorkers = workerCount;

    const worker = async () => {
      for (;;) {
        const index = cursor++;
        if (index >= queue.length) break;
        const task = queue[index];

        this.#active++;
        this.#peak = Math.max(this.#peak, this.#active);
        const result = await this.#runOne(task);
        this.#active--;

        this.#channel.push(result);
      }
      if (--liveWorkers === 0) this.#channel.close();
    };

    for (let i = 0; i < workerCount; i++) worker();
    return this;
  }

  async #runOne(task) {
    const { retries, timeout, backoff } = this.#opts;
    let attempt = 0;
    for (;;) {
      attempt++;
      try {
        const value = await withTimeout(
          Promise.resolve(task.run({ attempt, clock: this.#clock })),
          timeout,
          this.#clock,
        );
        return { id: task.id, ok: true, value, attempts: attempt };
      } catch (error) {
        if (attempt > retries) return { id: task.id, ok: false, error, attempts: attempt };
        const delay = backoff(attempt);
        if (delay > 0) await this.#clock.sleep(delay);
      }
    }
  }
}

/** Drive a scheduler to completion against a virtual clock, collecting results. */
async function runToCompletion(scheduler, clock) {
  const collected = [];
  const consume = (async () => {
    for await (const r of scheduler.results()) collected.push(r);
  })();
  scheduler.start();
  await clock.drain();
  await consume;
  return collected;
}

// --- tests -------------------------------------------------------------------

test("respects the concurrency limit", async () => {
  const clock = new VirtualClock();
  const sched = new Scheduler({ concurrency: 3, clock });
  for (let i = 0; i < 10; i++) {
    sched.add({ id: `t${i}`, run: () => clock.sleep(10).then(() => i) });
  }
  const results = await runToCompletion(sched, clock);
  assertEqual(results.length, 10);
  assert(sched.peakConcurrency <= 3, `peak ${sched.peakConcurrency} exceeded 3`);
  assert(results.every((r) => r.ok));
});

test("times out slow tasks", async () => {
  const clock = new VirtualClock();
  const sched = new Scheduler({ concurrency: 2, timeout: 50, clock });
  sched.add({ id: "fast", run: () => clock.sleep(10).then(() => "ok") });
  sched.add({ id: "slow", run: () => clock.sleep(1000).then(() => "never") });
  const results = await runToCompletion(sched, clock);
  const byId = Object.fromEntries(results.map((r) => [r.id, r]));
  assertEqual(byId.fast.ok, true);
  assertEqual(byId.fast.value, "ok");
  assertEqual(byId.slow.ok, false);
  assert(byId.slow.error instanceof TimeoutError);
});

test("retries with backoff until success", async () => {
  const clock = new VirtualClock();
  const sched = new Scheduler({
    concurrency: 1,
    retries: 3,
    backoff: (attempt) => attempt * 5, // 5, 10, 15ms
    clock,
  });
  let calls = 0;
  sched.add({
    id: "flaky",
    run: async ({ attempt }) => {
      calls++;
      if (attempt < 3) throw new Error("transient");
      return "recovered";
    },
  });
  const [r] = await runToCompletion(sched, clock);
  assertEqual(r.ok, true);
  assertEqual(r.value, "recovered");
  assertEqual(r.attempts, 3);
  assertEqual(calls, 3);
});

test("gives up after exhausting retries", async () => {
  const clock = new VirtualClock();
  const sched = new Scheduler({ concurrency: 1, retries: 2, backoff: () => 1, clock });
  sched.add({ id: "doomed", run: async () => { throw new Error("always fails"); } });
  const [r] = await runToCompletion(sched, clock);
  assertEqual(r.ok, false);
  assertEqual(r.attempts, 3); // 1 initial + 2 retries
  assertEqual(r.error.message, "always fails");
});

test("results arrive in completion order, not submission order", async () => {
  const clock = new VirtualClock();
  const sched = new Scheduler({ concurrency: 3, clock });
  sched.add({ id: "300", run: () => clock.sleep(300).then(() => 300) });
  sched.add({ id: "100", run: () => clock.sleep(100).then(() => 100) });
  sched.add({ id: "200", run: () => clock.sleep(200).then(() => 200) });
  const results = await runToCompletion(sched, clock);
  assertEqual(results.map((r) => r.id), ["100", "200", "300"]);
});

test("mixed success/failure aggregate", async () => {
  const clock = new VirtualClock();
  const sched = new Scheduler({ concurrency: 4, retries: 0, clock });
  for (let i = 0; i < 8; i++) {
    sched.add({
      id: `job${i}`,
      run: () => clock.sleep(10).then(() => {
        if (i % 3 === 0) throw new Error(`fail ${i}`);
        return i * i;
      }),
    });
  }
  const results = await runToCompletion(sched, clock);
  const ok = results.filter((r) => r.ok);
  const failed = results.filter((r) => !r.ok);
  assertEqual(ok.length, 5); // i = 1,2,4,5,7,8 -> wait, 0..7 non-mult-of-3
  assertEqual(failed.length, 3); // i = 0,3,6
  assertClose(ok.reduce((s, r) => s + r.value, 0), 1 + 4 + 16 + 25 + 49);
});

await run("promise scheduler");
