// flags: --std
import * as std from "std";
import { TCPServer, UDPSocket } from "dyna:net";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }

{
    function throwsMsgC6(fn, re, m) {
        let threw = null;
        try { fn(); } catch (e) { threw = e; }
        check(threw !== null && threw instanceof TypeError && re.test(threw.message),
              m + " (got " + (threw ? threw.name + ": " + threw.message : "no throw") + ")");
    }

    throwsMsgC6(() => TCPServer.connect({ host: "127.0.0.1", portz: 1 }, {}),
                /unknown option "portz" \(valid: path, host, port, maxConnections, idleTimeoutMs, connectTimeoutMs, tls, highWaterMark\)/,
                "connect options bag");
    throwsMsgC6(() => TCPServer.connect({ host: "127.0.0.1", port: 1 }, { onDat() {} }),
                /unknown option "onDat" \(valid: connect, data, close, drain\)/,
                "connect handlers bag");
    throwsMsgC6(() => new TCPServer({ port: 0, tls: { cert: "a", key: "b", requestCert2: true } }),
                /unknown option "requestCert2" \(valid: cert, key, alpn, ca, requestCert\)/,
                "server tls sub-bag");
    {
        const sx = new TCPServer({ port: 0 });
        let threw = null;
        try { sx.start({ onDat() {} }); } catch (e) { threw = e; }
        check(threw !== null && threw instanceof TypeError &&
              /unknown option "onDat" \(valid: connect, data, close, drain\)/.test(threw.message),
              "start handlers bag throws (got " + (threw ? threw.message : "no throw") + ")");
        check(sx.port === 0 || sx.port === undefined,
              "refused start never bound a port (got " + sx.port + ")");
        sx.close();
    }
    throwsMsgC6(() => new UDPSocket({ port: 0, hos: "127.0.0.1" }),
                /unknown option "hos" \(valid: port, host\)/,
                "UDPSocket ctor bag");
    {
        const u = new UDPSocket({ port: 0 });
        let uthrew = null;
        try { u.start({ onMessage() {} }); } catch (e) { uthrew = e; }
        check(uthrew !== null && uthrew instanceof TypeError &&
              /unknown option "onMessage" \(valid: message\)/.test(uthrew.message),
              "UDPSocket start bag throws (got " + (uthrew ? uthrew.message : "no throw") + ")");
        let armed = true;
        try { u.start({ message() {} }); } catch (e) { armed = false; }
        check(armed, "valid UDPSocket start bag still works");
        u.close();
    }
}

const dec = new TextDecoder();

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

let refusedErr = null, refusedOk = false;
const bad = TCPServer.connect({ host: "127.0.0.1", port: 1 }, {
  connect: (c, err) => { if (err) refusedErr = String(err); else refusedOk = true; },
});

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

const IPCP = `${std.getenv("TMPDIR") || "/tmp"}/dj_tcp.${Date.now() % 10000000}.sock`;
let iGot = null, iReply = null;
const isrv = new TCPServer({ path: IPCP });
isrv.start({ data: (c, b) => { iGot = dec.decode(b); c.write("ipc:" + iGot); } });
const icli = TCPServer.connect({ path: IPCP }, {
  connect: (c, err) => { if (!err) c.write("over-unix"); },
  data: (c, b) => { iReply = dec.decode(b); },
});

let f1Closed = false;
const f1srv = new TCPServer({ port: 0 });
f1srv.start({ connect: (c) => { f1srv.close(); f1Closed = true; } });
const f1cli = TCPServer.connect({ host: "127.0.0.1", port: f1srv.port,
                                  connectTimeoutMs: 2000 },
  { connect: () => {}, close: () => {} });

let sconn = null, drains = 0, refused = null, refusedQueued = -1;
let bigs = null, bigc = null, sconn_closed = false, drainsAtClose = -1;
let amounts = [], postDrainWriteOk = false, clientBytes = 0;
const HWM = 256 * 1024, CHUNK = 64 * 1024;

const bpsrv = new TCPServer({ port: 0, highWaterMark: HWM });
bpsrv.start({
  connect: (c) => {
    sconn = c;
    check(c.bufferedAmount === 0, "bufferedAmount starts at 0, got " + c.bufferedAmount);
    for (let i = 0; i < 100; i++) {
      try {
        c.write(new Uint8Array(CHUNK));
        amounts.push(c.bufferedAmount);
      } catch (e) {
        refused = e;
        refusedQueued = c.bufferedAmount;
        break;
      }
    }
  },
  drain: (c) => {
    drains++;
    if (drains === 1)
      postDrainWriteOk = (() => { try { c.write("ok-after-drain"); return true; } catch (e) { return false; } })();
  },
  data: () => {},
});
{
    const big = new TCPServer({ port: 0, highWaterMark: 1024 });
    big.start({ connect: (c) => {
      let threw = null;
      try { c.write(new Uint8Array(8192)); } catch (e) { threw = e; }
      check(threw instanceof TypeError && /high-water mark/.test(threw.message),
            "an 8 KiB write against a 1 KiB cap is refused on an empty queue, got " + threw);
      check(c.bufferedAmount === 0, "the refused write queued nothing");
    }});
    const bigcli = TCPServer.connect({ host: "127.0.0.1", port: big.port },
                                     { connect: () => {}, close: () => {} });
    bigs = big; bigc = bigcli;
}

let hwmErr = null;
try { new TCPServer({ port: 0, highWaterMark: 0 }); } catch (e) { hwmErr = String(e); }
check(hwmErr !== null && /highWaterMark must be/.test(hwmErr),
      "highWaterMark 0 is a RangeError, got " + hwmErr);
hwmErr = null;
try { TCPServer.connect({ host: "127.0.0.1", port: 1, highWaterMark: -5 }, {}); }
catch (e) { hwmErr = String(e); }
check(hwmErr !== null && /highWaterMark must be/.test(hwmErr),
      "a negative highWaterMark is a RangeError, got " + hwmErr);

const bpcli = TCPServer.connect({ host: "127.0.0.1", port: bpsrv.port }, {
  data: (c, b) => { clientBytes += b.length; },
});

let spins = 0;
const t = setInterval(() => {
  const done = (reply !== null && (refusedErr !== null || refusedOk) &&
                uGot !== null && uEmpty && iReply !== null && f1Closed &&
                refused !== null && drains > 0) ||
               spins++ > 1200;
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

  check(amounts.length >= 2, "the burst queued several chunks before refusing (" +
        amounts.length + " accepted)");
  let mono = true;
  for (let i = 1; i < amounts.length; i++)
    if (amounts[i] < amounts[i - 1]) mono = false;
  check(mono, "bufferedAmount is monotonic while the peer cannot drain");
  check(amounts[amounts.length - 1] + CHUNK > HWM,
        "the queue actually reached the high-water mark region (last=" +
        amounts[amounts.length - 1] + ", hwm=" + HWM + ")");
  check(refused instanceof TypeError && /high-water mark/.test(refused.message),
        "a write past the cap is REFUSED with a TypeError, got " + refused);
  check(refusedQueued === amounts[amounts.length - 1],
        "the refused write left bufferedAmount untouched (" + refusedQueued +
        " vs " + amounts[amounts.length - 1] + ")");
  check(drains === 1, "drain fires EXACTLY ONCE per emptying, got " + drains);
  check(postDrainWriteOk, "writing works again after drain");
  check(clientBytes > 0, "the peer received the queued bytes (" + clientBytes + ")");

  check(sconn && !sconn_closed, "the backpressure conn is still open");
  for (let i = 0; i < 100 && sconn.bufferedAmount === 0; i++) {
    try { sconn.write(new Uint8Array(64 * 1024)); }
    catch (e) { break; }
  }
  const queuedAtClose = sconn.bufferedAmount;
  check(queuedAtClose > 0, "bytes are actually queued at close (" + queuedAtClose + ")");
  sconn.close();
  sconn_closed = true;
  check(sconn.bufferedAmount === 0, "a closed conn reads bufferedAmount 0");
  let closedWrite = null;
  try { sconn.write("x"); } catch (e) { closedWrite = e; }
  check(closedWrite instanceof TypeError,
        "write after close is refused, got " + closedWrite);
  check(queuedAtClose >= 0, "queued-at-close observed (" + queuedAtClose + ")");
  drainsAtClose = drains;

  cli.close();
  bad.close();
  srv.close();
  check(drainsAtClose === drains,
        "no drain after the close-during-inflight (" + drainsAtClose + " -> " + drains + ")");
  bigc.close(); bigs.close();
  bpcli.close(); bpsrv.close();
  ucli.close(); usrv.close();
  icli.close(); isrv.close();
  check(f1Closed, "the connect handler ran and closed its server");
  f1cli.close(); f1srv.close();
  if (fails === 0) print("test_net_tcp: all " + n + " checks passed");
  else print("test_net_tcp: " + fails + " FAILED");
}, 10);
