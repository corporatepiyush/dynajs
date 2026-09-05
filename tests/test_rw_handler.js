import * as std from "std";
import * as os from "os";

function assert(actual, expected, message) {
    if (arguments.length == 1)
        expected = true;

    if (Object.is(actual, expected))
        return;

    if (actual !== null && expected !== null
    &&  typeof actual == 'object' && typeof expected == 'object'
    &&  actual.toString() === expected.toString())
        return;

    throw Error("assertion failed: got |" + actual + "|" +
                ", expected |" + expected + "|" +
                (message ? " (" + message + ")" : ""));
}

function handle_read(fd_r, fd_w, i)
{
    var buf = new Uint32Array(1);
    var val, len;
    len = os.read(fd_r, buf.buffer, 0, 4);
    os.setReadHandler(fd_r, null);
    val = buf[0];
//    print("read fd=", fd_r, "val=", val, "len=", len);
    assert(val, i);
}

function handle_write(fd_r, fd_w, i)
{
    var buf = new Uint32Array(1);
    buf[0] = i;
    os.write(fd_w, buf.buffer, 0, 4);
    os.setWriteHandler(fd_w, null);
}

function test_rw_handlers(n)
{
    var tab, fd_r, fd_w, i;
    tab = [];
    for(i = 0; i < n; i++) {
        tab[i] = os.pipe();
        fd_r = tab[i][0];
        fd_w = tab[i][1];
        os.setReadHandler(fd_r, handle_read.bind(null, fd_r, fd_w, i));
    }
    for(i = n - 1; i >= 0; i--) {
        fd_r = tab[i][0];
        fd_w = tab[i][1];
        os.setWriteHandler(fd_w, handle_write.bind(null, fd_r, fd_w, i));
    }
}

test_rw_handlers(100);

/* ---- P0-3 regression: os.close() must release read/write handler slots ---- */

/* Part A: close-without-clear must not leave the fd armed in the poll set.
   Before the fix the closed fd stayed in os_rw_handlers, poll() reported
   POLLNVAL, and the stale callback was re-dispatched forever (busy-spin) --
   here the callback never clears itself, so it would spin. After the fix,
   js_os_close drops the slot, so the callback is never invoked. The watchdog
   timer bounds the runtime: a busy-spin still wakes poll() immediately and the
   timer fires on schedule, at which point the staleCount assertion fails. */
function test_close_no_spin()
{
    var tab = os.pipe();
    var r = tab[0], w = tab[1];
    var staleCount = 0;

    os.setReadHandler(r, function() { staleCount++; });
    os.close(r);   /* WITHOUT clearing the handler slot */

    return new Promise(function(resolve, reject) {
        os.setTimeout(resolve, 250);   /* watchdog: resolves once the loop is live */
    }).then(function() {
        /* If the stale read handler were still armed, poll() would spin on
           POLLNVAL and staleCount would grow. A value of 0 proves the slot was
           released on close. */
        assert(staleCount, 0,
               "stale read callback fired after os.close (POLLNVAL spin)");
    });
}

/* Part B: fd-number reuse must not deliver the old callback to a foreign fd.
   Close a read end that had a handler, then re-open a pipe that reuses the
   same fd number and make it readable WITHOUT registering a new os handler
   (an "unrelated subsystem" -- the exact audit scenario). Before the fix the
   stale handler is still keyed at the reused fd number, so poll() sees the
   foreign fd readable and dispatches the OLD callback (wrong-callback cross-
   object delivery). After the fix js_os_close already dropped the slot, so
   nothing fires. The watchdog bounds the runtime. */
function test_fd_reuse()
{
    var tab = os.pipe();
    var r = tab[0], w = tab[1];
    var oldCount = 0;

    os.setReadHandler(r, function() { oldCount++; });
    os.close(r);   /* WITHOUT clearing the handler slot */

    var tab2 = os.pipe();
    var r2 = tab2[0], w2 = tab2[1];
    assert(r2, r, "expected new pipe read fd to reuse the closed fd number");

    /* Make the reused fd readable. In the broken build the stale read callback
       (still armed on this number) fires on the foreign descriptor and spins;
       in the fixed build no handler is registered so oldCount stays 0. */
    var buf = new Uint32Array(1);
    buf[0] = 7;
    os.write(w2, buf.buffer, 0, 4);

    return new Promise(function(resolve, reject) {
        os.setTimeout(resolve, 100);   /* watchdog: bounds the runtime */
    }).then(function() {
        assert(oldCount, 0, "stale callback fired on the reused (foreign) fd");
    });
}

test_close_no_spin()
    .then(test_fd_reuse)
    .then(function() { print("test_rw_handler: P0-3 regression OK"); })
    .catch(function(e) { print("test_rw_handler: P0-3 FAILED:", e, e && e.stack); std.exit(1); });
