// flags: --std
// timeout: 60
// covers: c:src/modules/sys/sys-spawn.c
// A non-CLOEXEC fd numbered above 1024 must not reach a child, on every spawn path:
//   os.exec (posix_spawn path), os.exec with uid/gid (the fork path: on macOS posix_spawn cannot set
//   credentials, so passing our own uid forces it without root), dyna:sys Exec and Spawn.
// Base leaked on the os.exec fork path on macOS: its close loop stopped at 1024 (and on Linux it
// ignored a failing close_range, and a glibc without posix_spawn_file_actions_addclosefrom_np capped
// its addclose list at 1024 too). Every path now runs dyn_close_fds_from(3) (src/dyna-io.c).
// The test re-runs itself under `ulimit -n 4096` so fd 2000 can be planted.
import * as std from "std";
import * as os from "os";
import { Exec, Spawn, getuid } from "dyna:sys";

const FD = 2000;
const probe = `if [ -e /dev/fd/${FD} ]; then echo LEAK; else echo CLEAN; fi`;

if (scriptArgs[1] !== "child") {
  const self = scriptArgs[0];
  const r = Exec("/bin/sh", ["-c", `ulimit -n 4096 && exec "$0" --std "$1" child`, std.getenv("DYNAJS_BIN") || "./dynajs", self],
    { timeoutMs: 50000 });
  std.out.puts(r.stdout);
  std.err.puts(r.stderr);
  std.exit(r.code);
}

let pass = 0, fail = 0;
const eq = (a, b, m) => { if (a === b) pass++; else { fail++; print("FAIL: " + m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b)); } };

const [pr, pw] = os.pipe();
eq(os.dup2(pr, FD), FD, "fd " + FD + " planted (needs RLIMIT_NOFILE > " + FD + ")");
eq(os.read(FD, new ArrayBuffer(0), 0, 0), 0, "planted fd is open in the parent");

function viaOsExec(opts) {
  const [r, w] = os.pipe();
  const pid = os.exec(["/bin/sh", "-c", probe], Object.assign({ blocking: false, stdout: w }, opts));
  os.close(w);
  const f = std.fdopen(r, "r");
  const out = f.readAsString().trim();
  f.close();
  os.waitpid(pid, 0);
  return out;
}

eq(viaOsExec({}), "CLEAN", "os.exec (posix_spawn path)");
eq(viaOsExec({ uid: getuid() }), "CLEAN", "os.exec with uid (fork path)");
eq(Exec("/bin/sh", ["-c", probe]).stdout.trim(), "CLEAN", "dyna:sys Exec");
eq(Exec("/bin/sh", ["-c", probe], { uid: getuid() }).stdout.trim(), "CLEAN", "dyna:sys Exec with uid");
{
  const p = new Spawn("/bin/sh", ["-c", probe]);
  let s = "";
  for (;;) {
    const b = new Uint8Array(64);
    const k = await p.stdout.read(b);
    if (k === 0) break;
    s += String.fromCharCode(...b.subarray(0, k));
  }
  await p.wait();
  eq(s.trim(), "CLEAN", "dyna:sys Spawn");
}
os.close(FD);
os.close(pr);
os.close(pw);

print(`test_spawn_fd_inherit: ${pass} passed, ${fail} failed`);
if (fail) std.exit(1);
