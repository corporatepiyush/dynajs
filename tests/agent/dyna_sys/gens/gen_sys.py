#!/usr/bin/env python3
"""gen_sys.py — dyna:sys probes: env scoping, Exec matrix (quoting, env,
timeout, maxBuffer, encodings), machine facts. Machine-fact truths are baked
from sysctl at generation time (the oracle)."""
import subprocess
from probe_lib import emit

IMP_SYS = '''import { Exec, Which, env, getEnv, setEnv, args, cwd, chDir,
         platform, pid, hostName, homeDir, cpuInfo, memInfo, loadAvg, uptime,
         diskUsage, memoryUsage } from "dyna:sys";
import { Path } from "dyna:file";
'''

def sysctl(name):
    return subprocess.run(["sysctl", "-n", name], capture_output=True, text=True).stdout.strip()

THREADS = sysctl("hw.logicalcpu")
MEMTOTAL = sysctl("hw.memsize")
MODEL = sysctl("machdep.cpu.brand_string") or "arm64"

emit("sys", "env", IMP_SYS, r'''
// env() snapshot and getEnv agree on a known var
const snap = env();
assert_eq(typeof snap, "object", "env() returns object");
assert_eq(snap.HOME, getEnv("HOME"), "env() snapshot matches getEnv for HOME");
assert_eq(getEnv("DYNASYS_NOPE_XYZ"), undefined, "unset var is undefined");

// setEnv visibility
setEnv("DYNASYS_A", "42");
assert_eq(getEnv("DYNASYS_A"), "42", "setEnv then getEnv");
assert_eq(env().DYNASYS_A, "42", "setEnv visible in env() snapshot");
setEnv("DYNASYS_A", "43");
assert_eq(getEnv("DYNASYS_A"), "43", "setEnv overwrites");

// refusals: empty name, '=' in name, NUL anywhere
assert_throws(() => setEnv("", "x"), null, "setEnv empty name refused");
assert_throws(() => setEnv("A=B", "x"), null, "setEnv name with '=' refused");
assert_throws(() => setEnv("A\u0000B", "x"), null, "setEnv NUL in name refused");
assert_throws(() => setEnv("DYNASYS_NUL", "v\u0000v"), null, "setEnv NUL in value refused");

// empty VALUE is legal
setEnv("DYNASYS_EMPTY", "");
assert_eq(getEnv("DYNASYS_EMPTY"), "", "empty value round-trips");
summary("sys.env");
''')

emit("sys", "exec", IMP_SYS, r'''
// stdout/stderr/code
const r1 = Exec("/bin/sh", ["-c", "echo out; echo err >&2"]);
assert_eq(r1.code, 0, "exit code 0");
assert_eq(r1.stdout, "out\n", "stdout exact");
assert_eq(r1.stderr, "err\n", "stderr exact");
assert_eq(r1.timedOut, false, "timedOut false");
assert_eq(r1.signal, null, "signal null on clean exit");

// exit code passthrough
const r2 = Exec("/bin/sh", ["-c", "exit 7"]);
assert_eq(r2.code, 7, "exit code passthrough");

// signal: code null, signal named
const r3 = Exec("/bin/sh", ["-c", "kill -TERM $$"]);
assert_eq(r3.code, null, "code null when signal killed the child");
assert_eq(r3.signal, "SIGTERM", "signal names the killer");

// argv fidelity: quoting matrix through argv (no shell anywhere)
const q = Exec("/usr/bin/printf", ["%s\\n", "a b", 'c"d', "e'f", "", "--x=1", "-", "--", "-y"]);
const parts = q.stdout.slice(0, -1).split("\n");
assert_eq(parts.length, 8, "8 argv elements echo as 8 lines, got " + parts.length);
assert_eq(parts[0], "a b", "embedded space survives argv");
assert_eq(parts[1], 'c"d', "embedded double quote survives");
assert_eq(parts[2], "e'f", "embedded single quote survives");
assert_eq(parts[3], "", "empty arg survives");
assert_eq(parts[4], "--x=1", "flag-looking arg survives");
assert_eq(parts[6], "--", "double-dash passes through (no shell)");

// input to stdin
const r4 = Exec("cat", [], { input: "hello-stdin" });
assert_eq(r4.stdout, "hello-stdin", "stdin input forwarded");

// encoding bytes
const r5 = Exec("/bin/sh", ["-c", "printf \'\\377\\200\\001\'"], { encoding: "bytes" });
assert_true(r5.stdout instanceof Uint8Array, "encoding bytes -> Uint8Array");
assert_eq(r5.stdout.length, 3, "binary bytes length 3");
assert_eq(r5.stdout[0], 255, "byte 0xFF survives");

// env replacement: child env EXACTLY the object
const r6 = Exec("/usr/bin/env", [], { env: { FOO: "bar", BAZ: "qux" } });
const lines = r6.stdout.trim().split("\n").sort();
assert_eq(lines.join("|"), "BAZ=qux|FOO=bar", "env replaces (not augments), got " + lines.join("|"));

// cwd option
const here = cwd();
const r7 = Exec("/bin/pwd", [], { cwd: "/private/tmp" });
assert_eq(r7.stdout.trim(), "/private/tmp", "cwd option sets child dir");

// timeout: SIGTERM then SIGKILL
const t0 = Date.now();
const r8 = Exec("/bin/sh", ["-c", "sleep 30"], { timeoutMs: 200 });
assert_eq(r8.timedOut, true, "timeout flags timedOut");
assert_eq(r8.signal, "SIGTERM", "timeout SIGTERMs the child");
assert_true(Date.now() - t0 < 5000, "timeout is prompt");

// maxBuffer: throw, not truncate
let mbErr = null;
try {
  Exec("/bin/sh", ["-c", "yes abcd | head -c 100000"], { maxBuffer: 1024 });
} catch (e) { mbErr = e; }
assert_true(mbErr !== null, "maxBuffer exceeded throws");

// missing command: clear error, not exit 127
let missErr = null;
try { Exec("definitely-not-a-command-xyz", []); } catch (e) { missErr = e; }
assert_true(missErr !== null, "missing command throws");

// PATH resolution happens against child's PATH when env replaced
const w = Which("sh");
assert_true(w !== null && w.startsWith("/"), "Which resolves against PATH: " + w);
assert_eq(Which("definitely-not-a-command-xyz"), null, "Which returns null when not found");
summary("sys.exec");
''')

emit("sys", "facts", IMP_SYS, r'''
import { Path as FP, writeFile } from "dyna:file";
assert_eq(platform(), "darwin", "platform is darwin on this host");
assert_true(pid() > 0, "pid positive");
assert_true(hostName().length > 0, "hostname non-empty");
assert_eq(getEnv("HOME"), homeDir(), "homeDir equals $HOME (set here)");

// cwd/chDir round-trip
const before = cwd();
assert_true(before.startsWith("/"), "cwd absolute");
chDir("/private/tmp");
assert_eq(cwd(), "/private/tmp", "chDir moves");
chDir(before);
assert_eq(cwd(), before, "chDir back");

// machine facts vs sysctl-baked truths (oracle computed at generation time)
const ci = cpuInfo();
assert_eq(ci.threads, __THREADS__, "cpuInfo.threads vs sysctl hw.logicalcpu");
assert_true(typeof ci.model === "string" && ci.model.length > 0, "cpuInfo.model present: " + ci.model);
assert_true(Array.isArray(ci.features), "cpuInfo.features array");
assert_true(ci.features.indexOf("neon") >= 0, "arm64 dispatches neon, got " + JSON.stringify(ci.features));

const mi = memInfo();
assert_eq(mi.total, __MEMTOTAL__, "memInfo.total matches sysctl hw.memsize");
assert_true(mi.free > 0 && mi.available > 0, "memInfo free/available positive");

const la = loadAvg();
assert_eq(la.length, 3, "loadAvg [1,5,15]");
for (const v of la) assert_true(v >= 0, "loadAvg entries non-negative");
assert_true(uptime() > 0 && uptime() < 100 * 365 * 86400, "uptime plausible");

// diskUsage on a real dir
const du = diskUsage(cwd());
assert_true(du.total > 0 && du.available > 0 && du.free > 0, "diskUsage fields positive");
assert_true(du.available <= du.free, "available <= free");

// memoryUsage ledger
const mu = memoryUsage();
assert_true(mu.mallocCount >= 0 && mu.peakRss > 0, "memoryUsage basics");
assert_eq(mu.nativeLimit, 0, "nativeLimit default 0 (uncapped)");
assert_true(mu.nativeSize >= 0, "nativeSize >= 0");

// argv[0] is the binary path
assert_true(args().length >= 2, "argv has binary + script");
assert_true(args()[0].endsWith("dynajs"), "argv[0] is the engine binary, got " + args()[0]);
summary("sys.facts");
'''.replace("__THREADS__", THREADS).replace("__MEMTOTAL__", MEMTOTAL))
print("gen_sys: 3 probes (threads=%s mem=%s)" % (THREADS, MEMTOTAL))
