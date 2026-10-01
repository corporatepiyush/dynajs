import { UDPSocket } from "dyna:net";
import * as netmod from "dyna:net";
import * as std from "std";

let fails = 0;
function ok(c, msg) { if (!c) { fails++; print("FAIL: " + msg); } }

const counter = netmod.swallowedHandlerThrows;
ok(typeof counter === "function", "net.swallowedHandlerThrows is a function");
const before = counter();

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
  std.exit(fails === 0 ? 0 : 1);
}

src.start({ message: (data, from) => {
  received++;
  if (received === 1) {
    throw new Error("boom-from-handler");
  }
  done();
} });

dst.start({ message: (data, from) => {
  src.send(new Uint8Array([data[0] + 1]), "127.0.0.1", srcPort);
} });

src.send(new Uint8Array([7]), "127.0.0.1", dstPort);
setTimeout(() => { src.send(new Uint8Array([8]), "127.0.0.1", dstPort); }, 300);
setTimeout(() => { done(); }, 3000);
