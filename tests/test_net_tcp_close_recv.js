/* test_net_tcp_close_recv.js -- P1a-2 regression: tcp_on_recv read `c->owner`
 * as its FIRST action, before the closed/detached guard every sibling entry
 * point carries (tcp_deliver, tls_flush, tcp_on_connect).
 *
 * The defect: when a TCP conn is closed (user close() or the dispose-time
 * detach that sets owner=NULL) while a recv completion for its fd is still
 * dequeued by the shared reactor, the completion re-enters tcp_on_recv. The
 * old code dereferenced c->owner before any guard -- a use-after-free read, a
 * wrong-callback dispatch, or a busy-spin on a POLLNVAL-style wakeup. The fix
 * evaluates the guard first and bails without touching the owner, exactly as
 * the siblings do.
 *
 * This test drives the closest JS-reachable shape: a client conn's echo
 * arrives, the client closes the conn from a TIMER, and the server keeps
 * feeding bytes so a further recv completion can land on the now-closed /
 * detached conn. The assertions:
 *   - no `data` callback fires after the conn was closed (a post-close
 *     deliver into tcp_on_recv on a detached conn is the exact UAF / spin);
 *   - no crash, no hang, no busy-spin;
 *   - the process reaches its own completion and exits cleanly.
 *
 * The race window is held open across hundreds of interval ticks of continued
 * server feed so a completion racing the close is far more likely to be caught
 * than in a single-shot close. A hard interval count is the watchdog: if the
 * loop spins (busy-spin) or never completes, the watchdog asserts and the run
 * does not reach the completion line -- observable as a FAIL / non-zero exit.
 *
 * The conn is closed from a TIMER, not from inside its own `data` handler,
 * because closing a conn from inside its own data callback is a separate,
 * pre-existing reactor hang in this engine (reproducible on the unmodified
 * baseline) that is outside the scope of P1a-2. Closing from a timer keeps the
 * loop progressing so the close/recv race is what is actually under test.
 */
import { TCPServer } from "dyna:net";

let fails = 0, n = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }

const enc = new TextEncoder();
const dec = new TextDecoder();

/* One long-lived server. It echoes, and it keeps a handle on the accepted
 * server-side conn so the test can keep feeding bytes to the client AFTER the
 * client closes -- which is what drives a recv completion into tcp_on_recv on
 * a closed/detached conn. */
const srv = new TCPServer({ port: 0 });
let srvConn = null;
let serverGot = 0;
srv.start({ data: (c, b) => { serverGot++; srvConn = c; c.write(enc.encode("reply")); } });

let cliData = 0;
let postCloseData = 0;      /* must stay 0 */
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
  if (spins > 4000) { clearInterval(wd); finalize(true); return; }  /* watchdog */
  /* close the client conn once its echo has arrived -- from the timer, so the
   * in-flight recv near the close is what races the detach */
  if (!closed && cliData >= 1) {
    closed = true;
    closedAt = spins;
    try { cli.close(); } catch (e) {}
  }
  /* keep the server writing into the (now closed) client fd */
  if (closed && srvConn) {
    try { srvConn.write(enc.encode("more-" + spins)); feeds++; } catch (e) {}
  }
  /* finish a little after the close so any misdirected completion is caught */
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
