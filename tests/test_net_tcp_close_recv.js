import { TCPServer } from "dyna:net";

let fails = 0, n = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }

const enc = new TextEncoder();
const dec = new TextDecoder();

const srv = new TCPServer({ port: 0 });
let srvConn = null;
let serverGot = 0;
srv.start({ data: (c, b) => { serverGot++; srvConn = c; c.write(enc.encode("reply")); } });

let cliData = 0;
let postCloseData = 0;
let closed = false;
let feeds = 0;

const cli = TCPServer.connect({ host: "127.0.0.1", port: srv.port }, {
  connect: (c) => { c.write(enc.encode("ping")); },
  data: (c, b) => {
    if (closed) { postCloseData++; print("  AFTER-CLOSE data len=" + b.length); }
    cliData++;
  },
});

let spins = 0;
let closedAt = 0;
const wd = setInterval(() => {
  spins++;
  if (spins > 4000) { clearInterval(wd); finalize(true); return; }
  if (!closed && cliData >= 1) {
    closed = true;
    closedAt = spins;
    try { cli.close(); } catch (e) {}
  }
  if (closed && srvConn) {
    try { srvConn.write(enc.encode("more-" + spins)); feeds++; } catch (e) {}
  }
  if (closed && spins >= closedAt + 200) finalize(false);
}, 5);

let finalized = false;
function finalize(watchdog) {
  if (finalized) return;
  finalized = true;
  clearInterval(wd);
  if (watchdog)
    check(false, "watchdog fired after " + spins + " intervals (busy-spin/hang); " +
          "cliData=" + cliData + " postClose=" + postCloseData + " feeds=" + feeds);
  check(cliData >= 1, "the client received its echo before the close, got " + cliData);
  check(closed, "the client conn was closed mid-stream");
  check(postCloseData === 0,
        "no data callback fired after close (post-close recv into tcp_on_recv) -- got " + postCloseData);
  check(feeds > 0, "the server kept feeding the closed fd, got " + feeds);
  check(feeds < 4000, "the feed did not spin without bound, got " + feeds);
  try { srvConn && srvConn.close(); } catch (e) {}
  try { cli.close(); } catch (e) {}
  try { srv.close(); } catch (e) {}
  if (fails === 0)
    print("test_net_tcp_close_recv: all " + n + " checks passed");
  else
    print("test_net_tcp_close_recv: " + fails + " FAILED of " + n);
}
