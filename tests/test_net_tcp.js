/* test_net_tcp.js -- dyna:net TCPServer, listening and connecting, on the shared reactor.
 *
 * The case that matters is a server AND a client in ONE process: they used to
 * each install their own reactor into a single-fd slot and silently overwrite
 * each other, so nothing was ever drained and every handler stayed silent.
 */
import { TCPServer, UDPSocket } from "dyna:net";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }

const dec = new TextDecoder();

/* ---- 1. round trip: server and client in the same process ---- */
let got = null, reply = null, connErr = null, closedSeen = false;

const srv = new TCPServer({ port: 0 });
srv.start({
  data: (c, bytes) => { got = dec.decode(bytes); c.write("echo:" + got); },
  close: () => { closedSeen = true; },
});
check(srv.port > 0, "port: 0 must resolve to an OS-assigned port, got " + srv.port);

const cli = TCPServer.connect({ host: "127.0.0.1", port: srv.port }, {
  connect: (c, err) => { if (err) { connErr = String(err); return; } c.write("hello"); },
  data: (c, bytes) => { reply = dec.decode(bytes); },
});

/* ---- 2. a refused connect must report an error, not silence ---- */
let refusedErr = null, refusedOk = false;
const bad = TCPServer.connect({ host: "127.0.0.1", port: 1 }, {
  connect: (c, err) => { if (err) refusedErr = String(err); else refusedOk = true; },
});

/* ---- 3. UDP: payload, peer address, and a zero-length datagram ---- */
let uGot = null, uFrom = null, uEmpty = false;
const usrv = new UDPSocket({ port: 0, host: "127.0.0.1" });
usrv.start({ message: (bytes, addr) => {
  const s = dec.decode(bytes);
  if (s === "") { uEmpty = true; return; }
  uGot = s; uFrom = addr;
}});
check(usrv.port > 0, "UDPSocket port: 0 must resolve, got " + usrv.port);
const ucli = new UDPSocket({ port: 0, host: "127.0.0.1" });
check(ucli.send("hello-udp", "127.0.0.1", usrv.port) === 9, "send returns the length");
check(ucli.send("", "127.0.0.1", usrv.port) === 0, "a zero-length send is legal");

/* ---- 4. IPC over AF_UNIX: same handlers, a path instead of a port ---- */
const IPCP = "/tmp/dyn_net_tcp_test.sock";
let iGot = null, iReply = null;
const isrv = new TCPServer({ path: IPCP });
isrv.start({ data: (c, b) => { iGot = dec.decode(b); c.write("ipc:" + iGot); } });
const icli = TCPServer.connect({ path: IPCP }, {
  connect: (c, err) => { if (!err) c.write("over-unix"); },
  data: (c, b) => { iReply = dec.decode(b); },
});

let spins = 0;
const t = setInterval(() => {
  const done = (reply !== null && (refusedErr !== null || refusedOk) &&
                uGot !== null && uEmpty && iReply !== null) || spins++ > 600;
  if (!done) return;
  clearInterval(t);

  check(got === "hello", "server received '" + got + "', want 'hello'");
  check(reply === "echo:hello", "client received '" + reply + "', want 'echo:hello'");
  check(connErr === null, "connect reported an error: " + connErr);
  check(refusedErr !== null && !refusedOk,
        "a connect to a closed port must report an error (err=" + refusedErr +
        ", success=" + refusedOk + ")");

  check(uGot === "hello-udp", "UDP payload '" + uGot + "', want 'hello-udp'");
  check(uFrom && uFrom.address === "127.0.0.1" && uFrom.port > 0,
        "the datagram's PEER ADDRESS must arrive, got " + JSON.stringify(uFrom));
  check(uEmpty, "a zero-length datagram is legal and must be delivered");
  check(iGot === "over-unix", "IPC server got '" + iGot + "'");
  check(iReply === "ipc:over-unix", "IPC client got '" + iReply + "'");

  cli.close();
  bad.close();     /* every reactor user must release, or the loop never exits */
  srv.close();
  ucli.close(); usrv.close();
  icli.close(); isrv.close();
  if (fails === 0) print("test_net_tcp: all " + n + " checks passed");
  else print("test_net_tcp: " + fails + " FAILED");
}, 10);
