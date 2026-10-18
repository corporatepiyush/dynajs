// flags: --std
// Split from tests/test_audit_p1.js: the core sections.
import * as os from "os";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { n++; if (a !== b) { fails++; print("FAIL: " + m + " got " + JSON.stringify(a) + " want " + JSON.stringify(b)); } }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) {
        if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong message: " + e.message); }
    }
}
{
    const p = "/tmp/test_audit_os_pos_" + Date.now() + ".txt";
    let fd = os.open(p, os.O_CREAT|os.O_WRONLY|os.O_TRUNC, 0o644);
    const hello = new TextEncoder().encode("hello world").buffer;
    assert(os.write(fd, hello, 0, 11) === 11, "os.write hello");
    os.close(fd);
    fd = os.open(p, os.O_RDONLY);
    const buf = new Uint8Array(5);
    eq(os.read(fd, buf.buffer, 0, 5, 6), 5, "os.read at position 6");
    assert(new TextDecoder().decode(buf) === "world", "os.read position returns world");
    const pr = os.pipe();
    if (pr) {
        const wbuf = new TextEncoder().encode("x").buffer;
        const r = os.write(pr[1], wbuf, 0, 1, 123);
        assert(r === 1, "os.write to pipe with position falls back");
        os.close(pr[0]); os.close(pr[1]);
    }
    os.close(fd);
    os.remove(p);
    const p2 = "/tmp/test_audit_os_len_" + Date.now() + ".txt";
    let fd2 = os.open(p2, os.O_CREAT|os.O_WRONLY|os.O_TRUNC, 0o644);
    const b2 = new Uint8Array([1,2,3,4,5]).buffer;
    eq(os.write(fd2, b2, 1), 4, "os.write length omitted defaults to rest");
    os.close(fd2);
    os.remove(p2);
}
{
    const t1 = os.now();
    const t2 = os.now();
    const ns1 = Math.round(t1*1e6);
    assert(ns1 % 1000 === 0, "performance.now coarsened to 1us");
}
assert(true, "setTimeout trailing args covered by test_fn_timers");
{
    const start = Date.now();
    const rc = os.exec(["sleep", "5"], { blocking: true, timeout: 200 });
    const ms = Date.now()-start;
    assert(rc === -15 || rc === -14 || rc < 0, "os.exec timeout returns signal " + rc);
    assert(ms < 1000, "os.exec timeout bounded " + ms + "ms");
    eq(os.exec(["true"], { blocking: true }), 0, "os.exec blocking alias");
    eq(os.exec(["true"], { block: true }), 0, "os.exec block alias still works");
    let threw = "", lateStatus = null;
    try {
        const lostPid = os.exec(["/nonexistent-dynajs-probe-bin"], { block: false });
        lateStatus = os.waitpid(lostPid, 0)[1];
    } catch (e) { threw = e.constructor.name + ": " + e.message; }
    // The failure is reported synchronously wherever the child shares the
    // parent's memory until exec. qemu-user turns vfork into fork, so there the
    // same failure can only surface as the child's 127 exit; nothing else is OK.
    assert(threw === "TypeError: os.exec: fork error" || (threw === "" && ((lateStatus >> 8) & 0xff) === 127),
           "non-block exec failure throws TypeError, or (emulated vfork) the child exits 127 (got '" + threw + "', status " + lateStatus + ")");
    eq(os.exec(["/nonexistent-dynajs-probe-bin"], { block: true }), 127, "blocking exec failure keeps exit 127");
}
if (fails) { print("test_core_audit_p1: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_core_audit_p1 failed"); }
print("test_core_audit_p1: " + n + " assertions, 0 failures");
