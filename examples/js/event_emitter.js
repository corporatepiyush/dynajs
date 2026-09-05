// event_emitter.js — a typed EventEmitter supporting on/once/off, wildcard
// listeners, and *weak* subscriptions that never keep their subscriber alive.
//
// Weak listeners hold a WeakRef to the subscribing object plus a method name;
// when the object is garbage-collected the listener is dropped (both lazily on
// the next emit and eagerly via a FinalizationRegistry). Collection is forced
// deterministically in the tests with std.gc(), so the WeakRef behaviour is
// actually verified rather than hand-waved.
//
// Engine features exercised: Map + Set (insertion-ordered dispatch), WeakRef,
// FinalizationRegistry, private fields/methods, and optional chaining.

import * as std from "std";
import { test, run, assert, assertEqual, assertThrows, deepEqual } from "./harness.js";

/**
 * @typedef {Object} Listener
 * @property {boolean} once
 * @property {boolean} weak
 * @property {Function} [fn]        strong handler
 * @property {WeakRef<object>} [ref] weak target
 * @property {string} [method]      method to call on the weak target
 * @property {Function} orig        identity used by off()
 */

const WILDCARD = "*";

export class EventEmitter {
  /** @type {Map<string, Set<Listener>>} */
  #listeners = new Map();
  #registry = new FinalizationRegistry((heldEntry) => {
    // Runs when a weak target is collected: drop the entry if still present.
    heldEntry.set?.delete(heldEntry.listener);
  });

  #bucket(event) {
    let set = this.#listeners.get(event);
    if (!set) this.#listeners.set(event, (set = new Set()));
    return set;
  }

  /** Subscribe. Returns an unsubscribe function. */
  on(event, fn, { once = false } = {}) {
    if (typeof fn !== "function") throw new TypeError("handler must be a function");
    const entry = { once, weak: false, fn, orig: fn };
    this.#bucket(event).add(entry);
    return () => this.off(event, fn);
  }

  once(event, fn) {
    return this.on(event, fn, { once: true });
  }

  /**
   * Subscribe weakly: the emitter holds only a WeakRef to `target`, so the
   * subscription does not prevent `target` from being collected.
   */
  onWeak(event, target, method) {
    if (typeof target?.[method] !== "function") {
      throw new TypeError(`target has no method ${method}`);
    }
    const set = this.#bucket(event);
    // NB: the entry must not strongly reference `target` anywhere (no `orig:
    // target`), or the WeakRef would never be collectable. off() matches weak
    // entries by dereferencing the WeakRef instead.
    const entry = { once: false, weak: true, ref: new WeakRef(target), method, orig: undefined };
    set.add(entry);
    this.#registry.register(target, { set, listener: entry }, entry);
    return () => this.off(event, target);
  }

  /** Remove listeners for `event` whose identity (fn or weak target) matches. */
  off(event, orig) {
    const set = this.#listeners.get(event);
    if (!set) return this;
    for (const entry of set) {
      const matches = entry.weak ? entry.ref.deref() === orig : entry.orig === orig;
      if (matches) {
        if (entry.weak) this.#registry.unregister(entry);
        set.delete(entry);
      }
    }
    if (set.size === 0) this.#listeners.delete(event);
    return this;
  }

  /** Fire `event`. Returns the number of listeners actually invoked. */
  emit(event, ...args) {
    let invoked = 0;
    invoked += this.#fire(this.#listeners.get(event), event, args, false);
    if (event !== WILDCARD) {
      invoked += this.#fire(this.#listeners.get(WILDCARD), event, args, true);
    }
    return invoked;
  }

  #fire(set, event, args, wildcard) {
    if (!set) return 0;
    let invoked = 0;
    // Snapshot first: handlers may unsubscribe (or emit) during dispatch.
    for (const entry of [...set]) {
      if (entry.weak) {
        const target = entry.ref.deref();
        if (target === undefined) {
          set.delete(entry); // collected: prune lazily
          continue;
        }
        wildcard ? target[entry.method](event, ...args) : target[entry.method](...args);
      } else {
        wildcard ? entry.fn(event, ...args) : entry.fn(...args);
        if (entry.once) set.delete(entry);
      }
      invoked++;
    }
    if (set.size === 0) this.#listeners.delete(event);
    return invoked;
  }

  listenerCount(event) {
    return this.#listeners.get(event)?.size ?? 0;
  }

  eventNames() {
    return [...this.#listeners.keys()];
  }
}

// --- tests -------------------------------------------------------------------

test("basic on/emit with multiple args", () => {
  const bus = new EventEmitter();
  const seen = [];
  bus.on("data", (a, b) => seen.push([a, b]));
  assertEqual(bus.emit("data", 1, 2), 1);
  assertEqual(bus.emit("data", 3, 4), 1);
  assertEqual(seen, [[1, 2], [3, 4]]);
  assertEqual(bus.emit("other"), 0); // no listeners
});

test("once fires exactly once then auto-removes", () => {
  const bus = new EventEmitter();
  let count = 0;
  bus.once("boot", () => count++);
  bus.emit("boot");
  bus.emit("boot");
  assertEqual(count, 1);
  assertEqual(bus.listenerCount("boot"), 0);
});

test("off and unsubscribe handle both work", () => {
  const bus = new EventEmitter();
  const fn = () => {};
  const unsub = bus.on("x", fn);
  assertEqual(bus.listenerCount("x"), 1);
  unsub();
  assertEqual(bus.listenerCount("x"), 0);

  bus.on("y", fn);
  bus.off("y", fn);
  assertEqual(bus.listenerCount("y"), 0);
});

test("wildcard listeners receive the event name", () => {
  const bus = new EventEmitter();
  const log = [];
  bus.on(WILDCARD, (evt, payload) => log.push(`${evt}:${payload}`));
  bus.on("click", () => {});
  const n = bus.emit("click", 5);
  assertEqual(n, 2); // the click handler + the wildcard
  bus.emit("hover", 9);
  assertEqual(log, ["click:5", "hover:9"]);
});

test("handler order is insertion order (Set semantics)", () => {
  const bus = new EventEmitter();
  const order = [];
  bus.on("e", () => order.push("a"));
  bus.on("e", () => order.push("b"));
  bus.on("e", () => order.push("c"));
  bus.emit("e");
  assertEqual(order, ["a", "b", "c"]);
});

test("input validation", () => {
  const bus = new EventEmitter();
  assertThrows(() => bus.on("e", 123), "must be a function");
  assertThrows(() => bus.onWeak("e", {}, "nope"), "no method");
});

test("weak listener is invoked while its target is alive", () => {
  const bus = new EventEmitter();
  const received = [];
  let subscriber = { onTick(n) { received.push(n); } };
  bus.onWeak("tick", subscriber, "onTick");
  bus.emit("tick", 1);
  bus.emit("tick", 2);
  assertEqual(received, [1, 2]);
  assertEqual(bus.listenerCount("tick"), 1);
  // keep `subscriber` referenced so it is not collected mid-test
  assert(subscriber !== null);
});

test("weak listener is pruned after its target is collected", () => {
  const bus = new EventEmitter();
  let calls = 0;
  let subscriber = { onGone() { calls++; } };
  bus.onWeak("ping", subscriber, "onGone");
  bus.emit("ping");
  assertEqual(calls, 1);

  subscriber = null; // drop the only strong reference
  std.gc(); // force collection deterministically

  const invoked = bus.emit("ping"); // deref() now undefined -> prune, don't call
  assertEqual(invoked, 0);
  assertEqual(calls, 1); // never called again
  assertEqual(bus.listenerCount("ping"), 0); // entry pruned
});

await run("event emitter");
