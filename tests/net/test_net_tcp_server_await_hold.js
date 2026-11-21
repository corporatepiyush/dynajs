// covers: dyna:net.TCPServer
// flags: --std
// timeout: 30
// Writes a handler makes after an await leave in one send too. The reactor runs the promise jobs a
// dispatch queued while it is still corked (dyn_aio_set_after_dispatch; dyna-net drains the runtime's
// job queue there), then uncorks and flushes, so `data: async (c) => { await x; c.write(a); c.write(b) }`
// sends a+b once instead of once per write; before, those jobs ran after the reactor returned and each
// write was its own send syscall (11 sends per round trip for 10 writes). Bounded: the hold lasts until
// the jobs the dispatch queued are drained, never across a poll; dyn_aio_run flushes anything held
// before it waits, so a job that pumps the reactor cannot wait on its own held bytes. Timer callbacks
// are not corked: a write there is one send, as before. A job that fails is re-queued as a job that
// rethrows, so the loop's error policy (print, exit) still applies.
// Pinned here: (1) after `await` in a data handler, bufferedAmount reports the held bytes (2, then 4)
// and the peer receives them with no further traffic; the flush happens before the dispatch returns, so
// a timer the job arms, which the loop runs before its next poll, sees bufferedAmount 0 (dyn_aio_run's
// own pre-poll flush would otherwise hide a reactor that skipped it); (2) two awaits deep, still held; (3) a write
// from a setTimeout callback is not held (bufferedAmount 0); (4) an owner with a drain handler is
// never held, also after an await.
import { TCPServer } from "dyna:net";
import * as std from "std";

import { suite } from "../lib/kit.js";

const t = suite("test_net_tcp_server_await_hold");
let A = null;
const check = (c, m) => A.ok(c, m);
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);

function run(withDrain, done) {
  const r = { awaited: [], deep: [], after: -1, timer: -1, text: "" };
  const handlers = {
    data: async (c, b) => {
      const s = String.fromCharCode(...b);
      if (s === "q") {
        await null;
        c.write("ab");
        r.awaited.push(c.bufferedAmount);
        c.write("cd");
        r.awaited.push(c.bufferedAmount);
        setTimeout(() => { if (r.after < 0) r.after = c.bufferedAmount; }, 0);
      } else if (s === "d") {
        await Promise.resolve();
        await null;
        c.write("ef");
        r.deep.push(c.bufferedAmount);
      } else if (s === "t") {
        setTimeout(() => { c.write("g"); r.timer = c.bufferedAmount; }, 0);
      }
    },
  };
  if (withDrain) handlers.drain = () => {};
  const srv = new TCPServer({ port: 0 });
  srv.start(handlers);
  const next = { 4: "d", 6: "t" };
  const cli = TCPServer.connect({ host: "127.0.0.1", port: srv.port }, {
    connect: (c) => c.write("q"),
    data: (c, b) => {
      r.text += String.fromCharCode(...b);
      if (next[r.text.length]) c.write(next[r.text.length]);
      else if (r.text.length >= 7) { c.close(); srv.close(); done(r); }
    },
  });
}

function verify(withDrain, r) {
  const L = withDrain ? "with a drain handler" : "without a drain handler";
  check(r.text === "abcdefg", L + ": every byte arrives in order with no further traffic (got " + JSON.stringify(r.text) + ")");
  if (withDrain) {
    check(r.awaited.join() === "0,0" && r.deep.join() === "0",
          L + ": writes after an await are not held (got " + r.awaited.join() + " / " + r.deep.join() + ")");
  } else {
    check(r.awaited.join() === "2,4",
          L + ": writes after an await in a data handler are held until the dispatch's jobs drain, bufferedAmount 2 then 4 (got "
          + r.awaited.join() + ")");
    check(r.deep.join() === "2", L + ": two awaits deep, still held (got " + r.deep.join() + ")");
    check(r.after === 0, L + ": the held bytes left before the dispatch returned: a timer the job armed sees "
          + "bufferedAmount 0, not bytes waiting for the next poll (got " + r.after + ")");
  }
  check(r.timer === 0, L + ": a write from a timer callback is one send, not held (bufferedAmount " + r.timer + ")");
}

test("without a drain handler", async () => {
  const r0 = await new Promise((res) => run(false, res));
  verify(false, r0);
}, { timeoutMs: 10000 });

test("with a drain handler", async () => {
  const r1 = await new Promise((res) => run(true, res));
  verify(true, r1);
}, { timeoutMs: 10000 });

await t.run();
