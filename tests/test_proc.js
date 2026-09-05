/* test_proc.js -- Exec and Which in dyna:sys (design 23).
 *
 * The cases that decide whether a subprocess API is correct are the ones a
 * happy-path test never reaches:
 *   - a child that writes MORE THAN A PIPE BUFFER holds, which deadlocks any
 *     implementation that waits for exit before draining;
 *   - a child that ignores SIGTERM, which is the only thing that proves the
 *     escalation to SIGKILL exists;
 *   - a child killed by a signal, which must NOT look like a nonzero exit;
 *   - and the absence of a shell, proved by passing metacharacters and
 *     requiring them back LITERALLY.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_proc.js
 */
import { Exec, Which, setEnv, getEnv } from "dyna:sys";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}

/* ------------------------------------------------------------------ basics */

{
    const r = Exec("echo", ["hello", "world"]);
    eq(r.code, 0, "a successful exit is code 0");
    eq(r.signal, null, "and no signal");
    eq(r.stdout, "hello world\n", "stdout is captured");
    eq(r.stderr, "", "stderr is empty");
    eq(r.timedOut, false, "and it did not time out");
}
eq(Exec("true").code, 0, "no args at all");
eq(Exec("false").code, 1, "a nonzero exit is reported");
eq(Exec("sh", ["-c", "exit 42"]).code, 42, "and the exact code");
eq(Exec("sh", ["-c", "echo out; echo err >&2"]).stderr, "err\n",
   "stderr is separate from stdout");
eq(Exec("sh", ["-c", "echo out; echo err >&2"]).stdout, "out\n", "and stdout is its own");

/* ---------------------------------------------------------- NO SHELL */

{
    /* THE INJECTION TEST. With a shell these would substitute; without one they
     * are ordinary characters in an argument. */
    const r = Exec("echo", ["$(whoami)", "`id`", "a;b", "a|b", "a>b", "*"]);
    eq(r.stdout, "$(whoami) `id` a;b a|b a>b *\n",
       "every metacharacter comes back LITERALLY -- there is no shell");
}
throws(() => Exec("echo hello"), "a command line with a space is not a command");
throws(() => Exec("echo", "hello"), "args must be an array, not a string");
throws(() => Exec("echo", [42]), "and every element must be a string");
throws(() => Exec(), "a command is required");
throws(() => Exec(42), "and it must be a string");
throws(() => Exec("definitely-not-a-real-command-xyz"),
       "a command that does not exist is an error, not a silent 127");

/* --------------------------------------------- THE DEADLOCK CASE */

{
    /* A child writing more than a pipe buffer (64 KiB on Linux, 16 KiB on
     * macOS) blocks until someone drains it. An implementation that waits for
     * exit first hangs here forever; this test either passes or never returns,
     * which is why the harness has a timeout around the suite. */
    const r = Exec("sh", ["-c", "i=0; while [ $i -lt 2000 ]; do "
                        + "echo 0123456789012345678901234567890123456789; "
                        + "i=$((i+1)); done"]);
    eq(r.code, 0, "a child that outruns the pipe buffer still completes");
    eq(r.stdout.length, 2000 * 41, "and every byte arrives (" + r.stdout.length + ")");
}
{
    /* Both pipes at once: draining only one is the other half of the deadlock. */
    const r = Exec("sh", ["-c", "i=0; while [ $i -lt 1000 ]; do "
                        + "echo out0123456789012345678901234567890123; "
                        + "echo err0123456789012345678901234567890123 >&2; "
                        + "i=$((i+1)); done"]);
    eq(r.code, 0, "a child filling BOTH pipes completes");
    eq(r.stdout.length, 1000 * 38, "stdout is whole");
    eq(r.stderr.length, 1000 * 38, "and so is stderr");
}

/* -------------------------------------------------------------- stdin */

eq(Exec("cat", [], { input: "piped in" }).stdout, "piped in", "input reaches the child");
eq(Exec("wc", ["-c"], { input: "12345" }).stdout.trim(), "5", "and all of it does");
{
    /* A child that never reads its stdin: the parent must not block on the
     * write, and must not die of SIGPIPE when the child exits first. */
    const r = Exec("true", [], { input: "x".repeat(200000) });
    eq(r.code, 0, "a child that ignores 200 KB of stdin does not hang the parent");
}
{
    const bytes = new Uint8Array([104, 105]);
    eq(Exec("cat", [], { input: bytes }).stdout, "hi", "input may be bytes");
}

/* ------------------------------------------------------------ timeout */

{
    const t0 = Date.now();
    const r = Exec("sleep", ["30"], { timeoutMs: 300 });
    const dt = Date.now() - t0;
    eq(r.timedOut, true, "a slow child times out");
    eq(r.signal, "SIGTERM", "and is asked to stop with SIGTERM first");
    eq(r.code, null, "a signalled child has NO exit code, which is not 0 either");
    assert(dt < 5000, "and it returns promptly (" + dt + " ms)");
}
{
    /* A child that IGNORES SIGTERM is the only case that proves the escalation
     * to SIGKILL is real -- against a well-behaved child both paths look the
     * same. `trap '' TERM` makes the shell ignore it outright. */
    const t0 = Date.now();
    const r = Exec("sh", ["-c", "trap '' TERM; sleep 30"], { timeoutMs: 300 });
    const dt = Date.now() - t0;
    eq(r.timedOut, true, "a child that ignores SIGTERM still times out");
    eq(r.signal, "SIGKILL", "and is escalated to SIGKILL");
    assert(dt < 8000, "within the grace period (" + dt + " ms)");
}
eq(Exec("echo", ["quick"], { timeoutMs: 30000 }).timedOut, false,
   "a fast child under a long timeout does not report a timeout");
throws(() => Exec("true", [], { timeoutMs: -1 }), "a negative timeout is refused");
{
    /* Huge timeouts: the deadline is now(CLOCK_MONOTONIC) + timeout, so a
     * timeout within ~uptime of INT64_MAX would wrap the add negative and
     * SIGTERM a healthy child instantly. The deadline saturates (defence for
     * long-uptime hosts -- the exact window depends on the host's uptime and
     * is not JS-reachable on a fresh boot, hence not directly assertable);
     * what IS assertable from JS: a huge-but-fitting timeout completes, and
     * a timeout beyond int64 is refused by validation rather than wrapping. */
    const t0 = Date.now();
    const r = Exec("true", [], { timeoutMs: 9e18 });
    const dt = Date.now() - t0;
    eq(r.code, 0, "a huge timeoutMs does not overflow into an instant SIGTERM");
    eq(r.timedOut, false, "and the child was not killed");
    eq(r.signal, null, "and no signal was sent");
    assert(dt < 5000, "and it returns promptly (" + dt + " ms)");
    throws(() => Exec("true", [], { timeoutMs: 1e19 }),
           "a timeout beyond int64 is refused (wrap would be negative)");
}

/* --------------------------------------------------------- signals */

{
    const r = Exec("sh", ["-c", "kill -9 $$"]);
    eq(r.signal, "SIGKILL", "a signalled child reports its signal by name");
    eq(r.code, null, "and its code is null, NOT a plausible small integer");
}

/* -------------------------------------------------------- maxBuffer */

{
    let msg = "";
    try {
        Exec("sh", ["-c", "i=0; while [ $i -lt 200 ]; do "
                  + "echo 0123456789012345678901234567890123456789; i=$((i+1)); done"],
             { maxBuffer: 100 });
    } catch (e) { msg = String(e.message); }
    assert(msg.indexOf("maxBuffer") >= 0,
           "output past maxBuffer is REFUSED, naming the limit (" + msg + ")");
}
eq(Exec("echo", ["ok"], { maxBuffer: 1024 }).stdout, "ok\n",
   "and a child under the limit is unaffected");
throws(() => Exec("true", [], { maxBuffer: 0 }), "maxBuffer must be positive");

/* ------------------------------------------------------- cwd and env */

{
    const r = Exec("pwd", [], { cwd: "/tmp" });
    assert(r.stdout.indexOf("tmp") >= 0, "cwd is honoured (" + r.stdout.trim() + ")");
}
throws(() => Exec("pwd", [], { cwd: "/definitely/not/a/directory" }),
       "an unusable cwd is not silently ignored");
{
    /* A replacement environment REPLACES: the parent's variables are gone. */
    const r = Exec("sh", ["-c", "echo \"$MY_VAR/$PATH\""],
                   { env: { MY_VAR: "set", PATH: "/bin:/usr/bin" } });
    eq(r.stdout, "set//bin:/usr/bin\n", "env replaces rather than merges");
}
{
    /* Which the child gets is the env's PATH, not the parent's -- execve does
     * not search, so the parent has to resolve against the right one. */
    const r = Exec("echo", ["found"], { env: { PATH: "/bin:/usr/bin" } });
    eq(r.code, 0, "a command is resolved against the replacement PATH");
}

/* ------------------------------------------------------------ encoding */

{
    const r = Exec("printf", ["\\101\\102"], { encoding: "bytes" });
    assert(r.stdout instanceof Uint8Array, "encoding: bytes gives a Uint8Array");
    eq(r.stdout.length, 2, "with the right length");
    eq(r.stdout[0], 65, "and the right bytes");
    assert(r.stderr instanceof Uint8Array, "stderr too");
}
throws(() => Exec("true", [], { encoding: "latin1" }), "an unknown encoding is refused");
throws(() => Exec("true", [], 42), "options must be an object");

/* --------------------------------------------------------------- Which */

{
    const sh = Which("sh");
    assert(typeof sh === "string" && sh.indexOf("/sh") > 0, "Which finds sh (" + sh + ")");
    eq(Exec(sh, ["-c", "echo via-which"]).stdout, "via-which\n",
       "and what it returns is runnable");
}
eq(Which("definitely-not-a-real-command-xyz"), null, "a missing command is null");
eq(Which(""), null, "an empty name is null, not the current directory");
eq(Which("/bin/sh"), "/bin/sh", "an absolute path is checked, not searched");
eq(Which("/definitely/not/here"), null, "and a bad one is null");
eq(Which("/etc"), null, "a directory is not an executable");
throws(() => Which(), "a name is required");
throws(() => Which(42), "and it must be a string");

{
    /* The execute-bit check: access(X_OK) answers 0 to ROOT for almost any
     * regular file, so the PATH walk must consult the mode bits itself. A
     * non-executable file in PATH is not a command; an executable one still is. */
    const dir = "/tmp/dyn_proc_which_" + Date.now();
    Exec("mkdir", ["-p", dir]);
    Exec("sh", ["-c", "printf '#!/bin/sh\\necho tool\\n' > \"$1/mytool\"; chmod 755 \"$1/mytool\"", "sh", dir]);
    Exec("sh", ["-c", "printf 'plain data' > \"$1/datafile\"; chmod 644 \"$1/datafile\"", "sh", dir]);
    const old = getEnv("PATH");
    setEnv("PATH", dir);
    eq(Which("mytool"), dir + "/mytool", "an executable file in PATH is found");
    eq(Which("datafile"), null,
       "a non-executable file in PATH is not a command, even for root");
    setEnv("PATH", old);
    assert(typeof Which("sh") === "string", "PATH restored: sh is found again");
    Exec("rm", ["-rf", dir]);
}

/* ------------------------------------------------------- uid and gid */

{
    /* Validation first: ids are non-negative integers that fit uid_t. */
    let threw = null;
    try { Exec("true", [], { uid: -1 }); } catch (e) { threw = e; }
    assert(threw && String(threw.message).indexOf("uid") >= 0,
           "a negative uid is refused (" + (threw && threw.message) + ")");
    threw = null;
    try { Exec("true", [], { gid: -1 }); } catch (e) { threw = e; }
    assert(threw && String(threw.message).indexOf("gid") >= 0,
           "a negative gid is refused (" + (threw && threw.message) + ")");
}
{
    /* Drop to the caller's OWN ids: a no-op for a normal user, and the one
     * case that is deterministic without privileges. The engine exports no
     * getuid(), so the first child tells us who we are. */
    const me = parseInt(Exec("id", ["-u"]).stdout.trim(), 10);
    const r = Exec("id", ["-u"], { uid: me });
    eq(r.code, 0, "spawning with uid = our own id succeeds");
    eq(r.stdout.trim(), String(me), "and the child ran as that uid");
    const mygid = parseInt(Exec("id", ["-g"]).stdout.trim(), 10);
    const rg = Exec("id", ["-g"], { gid: mygid });
    eq(rg.code, 0, "spawning with gid = our own gid succeeds");
    eq(rg.stdout.trim(), String(mygid), "and the child ran as that gid");
}

/* ---------------------------------------------- no fd leak across calls */

{
    /* One leaked descriptor per call is invisible at the default limit and
     * fatal at a low one, so the run is what exposes it -- 200 calls would
     * exhaust a typical soft limit if each leaked three pipes. */
    let ok = 0;
    for (let i = 0; i < 200; i++)
        if (Exec("true").code === 0) ok++;
    eq(ok, 200, "200 sequential children all run: no descriptor leak");
}

if (fails) {
    print("test_proc: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_proc failed");
}
print("test_proc: " + n + " assertions, 0 failures");
