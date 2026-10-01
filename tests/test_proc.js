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

{
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

{
    const r = Exec("sh", ["-c", "i=0; while [ $i -lt 2000 ]; do "
                        + "echo 0123456789012345678901234567890123456789; "
                        + "i=$((i+1)); done"]);
    eq(r.code, 0, "a child that outruns the pipe buffer still completes");
    eq(r.stdout.length, 2000 * 41, "and every byte arrives (" + r.stdout.length + ")");
}
{
    const r = Exec("sh", ["-c", "i=0; while [ $i -lt 1000 ]; do "
                        + "echo out0123456789012345678901234567890123; "
                        + "echo err0123456789012345678901234567890123 >&2; "
                        + "i=$((i+1)); done"]);
    eq(r.code, 0, "a child filling BOTH pipes completes");
    eq(r.stdout.length, 1000 * 38, "stdout is whole");
    eq(r.stderr.length, 1000 * 38, "and so is stderr");
}

eq(Exec("cat", [], { input: "piped in" }).stdout, "piped in", "input reaches the child");
eq(Exec("wc", ["-c"], { input: "12345" }).stdout.trim(), "5", "and all of it does");
{
    const r = Exec("true", [], { input: "x".repeat(200000) });
    eq(r.code, 0, "a child that ignores 200 KB of stdin does not hang the parent");
}
{
    const bytes = new Uint8Array([104, 105]);
    eq(Exec("cat", [], { input: bytes }).stdout, "hi", "input may be bytes");
}

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

{
    const r = Exec("sh", ["-c", "kill -9 $$"]);
    eq(r.signal, "SIGKILL", "a signalled child reports its signal by name");
    eq(r.code, null, "and its code is null, NOT a plausible small integer");
}

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

{
    const r = Exec("pwd", [], { cwd: "/tmp" });
    assert(r.stdout.indexOf("tmp") >= 0, "cwd is honoured (" + r.stdout.trim() + ")");
}
throws(() => Exec("pwd", [], { cwd: "/definitely/not/a/directory" }),
       "an unusable cwd is not silently ignored");
{
    const r = Exec("sh", ["-c", "echo \"$MY_VAR/$PATH\""],
                   { env: { MY_VAR: "set", PATH: "/bin:/usr/bin" } });
    eq(r.stdout, "set//bin:/usr/bin\n", "env replaces rather than merges");
}
{
    const r = Exec("echo", ["found"], { env: { PATH: "/bin:/usr/bin" } });
    eq(r.code, 0, "a command is resolved against the replacement PATH");
}

{
    const r = Exec("printf", ["\\101\\102"], { encoding: "bytes" });
    assert(r.stdout instanceof Uint8Array, "encoding: bytes gives a Uint8Array");
    eq(r.stdout.length, 2, "with the right length");
    eq(r.stdout[0], 65, "and the right bytes");
    assert(r.stderr instanceof Uint8Array, "stderr too");
}
throws(() => Exec("true", [], { encoding: "latin1" }), "an unknown encoding is refused");
throws(() => Exec("true", [], 42), "options must be an object");

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

{
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
    const me = parseInt(Exec("id", ["-u"]).stdout.trim(), 10);
    const r = Exec("id", ["-u"], { uid: me });
    eq(r.code, 0, "spawning with uid = our own id succeeds");
    eq(r.stdout.trim(), String(me), "and the child ran as that uid");
    const mygid = parseInt(Exec("id", ["-g"]).stdout.trim(), 10);
    const rg = Exec("id", ["-g"], { gid: mygid });
    eq(rg.code, 0, "spawning with gid = our own gid succeeds");
    eq(rg.stdout.trim(), String(mygid), "and the child ran as that gid");
}

{
    let ok = 0;
    for (let i = 0; i < 200; i++)
        if (Exec("true").code === 0) ok++;
    eq(ok, 200, "200 sequential children all run: no descriptor leak");
}

{
    throws(() => Exec("/bin/echo\0-evil", ["ok"]),
           "a NUL in the command is refused, not executed by its prefix");
    throws(() => Exec("echo", ["a\0b"]),
           "a NUL in an argument is refused (it would arrive as just \"a\")");
    throws(() => Exec("echo", [], { env: { "A\0B": "v" } }),
           "a NUL in an env NAME is refused (it would arrive as just A)");
    throws(() => Exec("echo", [], { env: { A: "v\0w" } }),
           "a NUL in an env VALUE is refused (it would arrive as just v)");
    throws(() => Which("echo\0x"), "Which refuses a NUL-bearing name");
    eq(Exec("echo", ["still-working"]).stdout, "still-working\n",
       "the engine survives the refusals and keeps spawning");
}

{
    const r = Exec("echo", ["z".repeat(400 * 1024), "y".repeat(400 * 1024),
                            "x".repeat(400 * 1024), "w".repeat(400 * 1024)],
                   { encoding: "bytes" });
    assert(r.code === 0 || (r.code === 127 && r.signal === null),
           "an oversized argv fails cleanly (code=" + r.code +
           " signal=" + r.signal + ")");
    assert(r.timedOut === false, "and does not hang the parent");
}

{
    for (const bad of [NaN, Infinity, -Infinity]) {
        throws(() => Exec("true", [], { timeoutMs: bad }),
               "timeoutMs " + String(bad) + " is refused, not a silent hang");
        throws(() => Exec("true", [], { uid: bad }),
               "uid " + String(bad) + " is refused, not a silent uid 0");
        throws(() => Exec("true", [], { gid: bad }),
               "gid " + String(bad) + " is refused");
        throws(() => Exec("true", [], { maxBuffer: bad }),
               "maxBuffer " + String(bad) + " is refused");
    }
    eq(Exec("echo", ["ok"], { timeoutMs: 30000.9, maxBuffer: 1024.9 }).stdout,
       "ok\n", "fractional timeoutMs/maxBuffer truncate and run");
}

if (fails) {
    print("test_proc: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_proc failed");
}
print("test_proc: " + n + " assertions, 0 failures");
