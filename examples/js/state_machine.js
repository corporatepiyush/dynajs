// state_machine.js — a small statechart engine with guards, entry/exit actions,
// and context, whose transition loop is driven by a two-way generator.
//
// The generator receives events via `.next(event)` and yields the resulting
// state snapshot, so the driver reads like a coroutine. A `Machine` façade wraps
// the generator for ergonomic `.send()` use.
//
// Engine features exercised: generators with two-way communication (`yield`
// returning the pushed value), frozen enums, guard closures, structural
// snapshots, and optional chaining.

import { test, run, assert, assertEqual, assertThrows, deepEqual } from "./harness.js";

/**
 * @typedef {Object} Transition
 * @property {string} target
 * @property {(ctx:any, event:any) => boolean} [guard]
 * @property {(ctx:any, event:any) => any} [action]  returns a context patch
 */

/**
 * The generator that actually runs a machine definition. It is a coroutine:
 * each `yield` emits the current snapshot and pauses; the value passed to the
 * next `.next(event)` becomes the event to process.
 */
export function* machineCoroutine(def) {
  let state = def.initial;
  let context = { ...(def.context ?? {}) };
  const enter = (s, event) => {
    const patch = def.states[s]?.entry?.(context, event);
    if (patch) context = { ...context, ...patch };
  };

  enter(state, { type: "@@init" });

  // Fields merged into the *next* emitted snapshot (e.g. a `rejected` marker).
  // Using a carry variable keeps the loop to exactly one `yield` per event, so
  // the generator never desyncs with the driver's `.next(event)` calls.
  let extra = {};

  for (;;) {
    const event = yield { state, context: { ...context }, done: isFinal(def, state), ...extra };
    extra = {};
    if (event === undefined) continue; // a bare .next() just re-reads state
    const node = def.states[state];
    const candidates = node?.on?.[event.type];
    const list = candidates === undefined ? [] : Array.isArray(candidates) ? candidates : [candidates];

    let taken = null;
    for (const t of list) {
      const tr = typeof t === "string" ? { target: t } : t;
      if (tr.guard && !tr.guard(context, event)) continue;
      taken = tr;
      break;
    }

    if (!taken) {
      // No enabled transition: stay put but flag it on the next snapshot.
      extra = { rejected: event.type };
      continue;
    }

    // exit -> action -> enter
    node?.exit?.(context, event);
    if (taken.action) {
      const patch = taken.action(context, event);
      if (patch) context = { ...context, ...patch };
    }
    state = taken.target;
    enter(state, event);
  }
}

function isFinal(def, state) {
  return def.states[state]?.final === true;
}

/** Ergonomic façade over the coroutine. */
export class Machine {
  #gen;
  #snapshot;

  constructor(def) {
    validate(def);
    this.#gen = machineCoroutine(def);
    this.#snapshot = this.#gen.next().value; // prime to the initial state
  }

  get state() { return this.#snapshot.state; }
  get context() { return this.#snapshot.context; }
  get done() { return this.#snapshot.done; }

  /** Send an event object (or a bare string, coerced to { type }). */
  send(event) {
    const evt = typeof event === "string" ? { type: event } : event;
    this.#snapshot = this.#gen.next(evt).value;
    return this.#snapshot;
  }

  /** Feed a sequence of events, returning the list of visited states. */
  run(events) {
    const trail = [this.state];
    for (const e of events) {
      this.send(e);
      trail.push(this.state);
    }
    return trail;
  }
}

function validate(def) {
  if (!def.initial || !def.states?.[def.initial]) {
    throw new Error("machine needs a valid `initial` state");
  }
  for (const [name, node] of Object.entries(def.states)) {
    for (const [evt, t] of Object.entries(node.on ?? {})) {
      const list = Array.isArray(t) ? t : [t];
      for (const tr of list) {
        const target = typeof tr === "string" ? tr : tr.target;
        if (!def.states[target]) {
          throw new Error(`state "${name}" event "${evt}" targets unknown state "${target}"`);
        }
      }
    }
  }
}

// --- example machines --------------------------------------------------------

/** A coin-operated turnstile. */
function turnstile() {
  return new Machine({
    initial: "locked",
    context: { coins: 0, pushes: 0 },
    states: {
      locked: {
        on: {
          COIN: { target: "unlocked", action: (ctx) => ({ coins: ctx.coins + 1 }) },
          PUSH: { target: "locked", action: (ctx) => ({ pushes: ctx.pushes + 1 }) },
        },
      },
      unlocked: {
        on: {
          PUSH: { target: "locked" },
          COIN: { target: "unlocked", action: (ctx) => ({ coins: ctx.coins + 1 }) },
        },
      },
    },
  });
}

/** A document review workflow with a guard on the approver role. */
function reviewWorkflow() {
  return new Machine({
    initial: "draft",
    context: { revisions: 0 },
    states: {
      draft: { on: { SUBMIT: "review" } },
      review: {
        on: {
          APPROVE: { target: "approved", guard: (_ctx, e) => e.role === "editor" },
          REJECT: { target: "draft", action: (ctx) => ({ revisions: ctx.revisions + 1 }) },
        },
      },
      approved: { on: { PUBLISH: "published" } },
      published: { final: true },
    },
  });
}

// --- tests -------------------------------------------------------------------

test("turnstile transitions and context accumulation", () => {
  const t = turnstile();
  assertEqual(t.state, "locked");
  t.send("COIN");
  assertEqual(t.state, "unlocked");
  assertEqual(t.context.coins, 1);
  t.send("PUSH");
  assertEqual(t.state, "locked");
  const trail = t.run(["PUSH", "COIN", "COIN", "PUSH"]);
  assertEqual(trail, ["locked", "locked", "unlocked", "unlocked", "locked"]);
  assertEqual(t.context.coins, 3);
  assertEqual(t.context.pushes, 1);
});

test("guarded transitions block unless condition holds", () => {
  const wf = reviewWorkflow();
  wf.send("SUBMIT");
  assertEqual(wf.state, "review");

  // Wrong role: guard fails, transition rejected, state unchanged.
  const rejected = wf.send({ type: "APPROVE", role: "intern" });
  assertEqual(rejected.rejected, "APPROVE");
  assertEqual(wf.state, "review");

  // Correct role: approved.
  wf.send({ type: "APPROVE", role: "editor" });
  assertEqual(wf.state, "approved");
});

test("reject loops back and counts revisions", () => {
  const wf = reviewWorkflow();
  wf.send("SUBMIT");
  wf.send({ type: "REJECT" });
  assertEqual(wf.state, "draft");
  assertEqual(wf.context.revisions, 1);
  wf.send("SUBMIT");
  wf.send({ type: "REJECT" });
  assertEqual(wf.context.revisions, 2);
});

test("reaches a final state", () => {
  const wf = reviewWorkflow();
  const trail = wf.run([
    "SUBMIT",
    { type: "APPROVE", role: "editor" },
    "PUBLISH",
  ]);
  assertEqual(trail, ["draft", "review", "approved", "published"]);
  assert(wf.done);
});

test("unknown events are reported, not thrown", () => {
  const t = turnstile();
  const snap = t.send("TELEPORT");
  assertEqual(snap.rejected, "TELEPORT");
  assertEqual(t.state, "locked");
});

test("invalid definitions are rejected at construction", () => {
  assertThrows(() => new Machine({ initial: "nope", states: {} }), "valid `initial`");
  assertThrows(
    () => new Machine({ initial: "a", states: { a: { on: { GO: "ghost" } } } }),
    "unknown state",
  );
});

test("coroutine can be driven directly", () => {
  const gen = machineCoroutine({
    initial: "s0",
    states: { s0: { on: { NEXT: "s1" } }, s1: { on: { NEXT: "s0" } } },
  });
  assertEqual(gen.next().value.state, "s0"); // prime
  assertEqual(gen.next({ type: "NEXT" }).value.state, "s1");
  assertEqual(gen.next({ type: "NEXT" }).value.state, "s0");
});

await run("state machine");
