// FIX3 probe: swallowed net-handler throws are observable.
// Doctrine (unchanged): a throwing UDP message handler must NOT tear down the
// loop -- the process still exits rc=0. New surface: every swallow increments
// net.swallowedHandlerThrows(). Under -d, the first throw of each handler kind
// also prints a one-line stderr diagnostic, and the -d dump ends with
// "net swallowed handler throws: N".
//
// Node-comparable? No -- dyna:net is dynajs-only (like the other dyna_sys
// probes). Expectations: RESULT PASS via the counter arithmetic below.
import { UDPSocket } from "dyna:net";
import * as netmod from "dyna:net";
import * as std from "std";

let fails = 0;
function ok(c, msg) { if (!c) { fails++; print("FAIL: " + msg); } }

const counter = netmod.swallowedHandlerThrows;
ok(typeof counter === "function", "net.swallowedHandlerThrows is a function");
const before = counter();

// Loopback pair: dst bounces datagrams back to src; src throws on the first
// bounce, processes the second normally.
const dst = new UDPSocket({ port: 0, host: "127.0.0.1" });
const dstPort = dst.port;
const src = new UDPSocket({ port: 0, host: "127.0.0.1" });
var srcPort = src.port;

var received = 0;
var finished = false;
function done() {
  if (finished) return;
  finished = true;
  ok(received === 2, "second datagram still delivered after a handler throw (got " + received + ")");
  const after = counter();
  ok(after === before + 1, "counter incremented by exactly 1 (" + before + " -> " + after + ")");
  if (fails === 0) print("RESULT PASS");
  else print("RESULT FAIL (" + fails + ")");
  // the live sockets keep the event loop fed forever; exit explicitly
  std.exit(fails === 0 ? 0 : 1);
}

src.start({ message: (data, from) => {
  received++;
  if (received === 1) {
    // swallowed: the loop must survive this and everything after it
    throw new Error("boom-from-handler");
  }
  done();
} });

dst.start({ message: (data, from) => {
  src.send(new Uint8Array([data[0] + 1]), "127.0.0.1", srcPort);
} });

// datagram 1: bounced back to src, whose handler throws.
src.send(new Uint8Array([7]), "127.0.0.1", dstPort);
// datagram 2: sent only after the throw has happened (a timer proves the
// loop is still live), bounced back and processed normally.
setTimeout(() => { src.send(new Uint8Array([8]), "127.0.0.1", dstPort); }, 300);
// failsafe: never hang the suite
setTimeout(() => { done(); }, 3000);
