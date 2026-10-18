// flags: --std
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

function test_close_no_spin()
{
    var tab = os.pipe();
    var r = tab[0], w = tab[1];
    var staleCount = 0;

    os.setReadHandler(r, function() { staleCount++; });
    os.close(r);

    return new Promise(function(resolve, reject) {
        os.setTimeout(resolve, 250);
    }).then(function() {
        assert(staleCount, 0,
               "stale read callback fired after os.close (POLLNVAL spin)");
    });
}

function test_fd_reuse()
{
    var tab = os.pipe();
    var r = tab[0], w = tab[1];
    var oldCount = 0;

    os.setReadHandler(r, function() { oldCount++; });
    os.close(r);

    var tab2 = os.pipe();
    var r2 = tab2[0], w2 = tab2[1];
    assert(r2, r, "expected new pipe read fd to reuse the closed fd number");

    var buf = new Uint32Array(1);
    buf[0] = 7;
    os.write(w2, buf.buffer, 0, 4);

    return new Promise(function(resolve, reject) {
        os.setTimeout(resolve, 100);
    }).then(function() {
        assert(oldCount, 0, "stale callback fired on the reused (foreign) fd");
    });
}

test_close_no_spin()
    .then(test_fd_reuse)
    .then(function() { print("test_rw_handler: P0-3 regression OK"); })
    .catch(function(e) { print("test_rw_handler: P0-3 FAILED:", e, e && e.stack); std.exit(1); });
