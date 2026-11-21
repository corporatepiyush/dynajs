// tests/lib/helper.js -- start a background helper (a mock server, a fake
// peer, a child dynajs) that can NEVER outlive the suite that started it.
// Works with and without --std (it needs only dyna:sys).
//
//   import { spawnHelper } from "../lib/helper.js";
//   const h = spawnHelper(`./dynajs --std ${P("mock.js")} ${P("port")} >${P("mock.log")} 2>&1`);
//   h.pid                // the helper's own pid (a simple command is exec'd)
//   h.stop();            // optional: deterministic stop, idempotent
//
//   spawnHelper(cmd, { pidFile: P("srv.pid") })   writes h.pid there first
//   spawnHelper(cmd, { cwd: dir })                runs cmd in dir
//   spawnHelper(cmd, { cleanup: `rm -rf ${T}` })  shell run once the helper is gone
//   cleanupOnExit(`rm -rf ${T}`)                  just the cleanup, no helper
//
// The launcher runs under bash: dash (Debian /bin/sh) has no job control
// without a terminal, so its `set -m` makes no group and the helper could not
// be reaped as one. The command is a shell string run in its OWN process group (stdin, and any
// output it does not redirect, go to /dev/null). A watcher shell in THIS
// process's group polls this process (kill -0, every 0.5 s); however the
// suite ends -- normal exit, a thrown error, a timeout, SIGKILL -- the watcher
// then TERMs the helper's whole group (KILL after 3 s) and runs the cleanup.
// A TERM/HUP/INT sent to the watcher (stop(), or a runner killing the suite's
// process group) does the same at once. The event loop is not held open.
//
// Never background a helper with sh(`... &`): that helper is reparented to
// pid 1 the moment the shell returns and lives until someone kills it by
// hand (tools/repo-rules.js: test-background-helper).
import { Exec, pid as selfPid } from "dyna:sys";

const LAUNCH = [
    'cmd=$1 pidfile=$2 cleanup=$3 owner=$4',
    'set -m',
    '( exec /bin/sh -c "$cmd" ) </dev/null >/dev/null 2>&1 &',
    'c=$!',
    'set +m',
    '[ -n "$pidfile" ] && echo "$c" > "$pidfile"',
    '(',
    '  z() { trap "" TERM HUP INT; kill -TERM -$c 2>/dev/null; i=0; while kill -0 -$c 2>/dev/null && [ $i -lt 30 ]; do sleep 0.1; i=$((i+1)); done; kill -KILL -$c 2>/dev/null; [ -n "$cleanup" ] && /bin/sh -c "$cleanup"; exit 0; }',
    '  trap z TERM HUP INT',
    '  while kill -0 "$owner" 2>/dev/null; do sleep 0.5 & wait $!; done',
    '  z',
    ') </dev/null >/dev/null 2>&1 &',
    'echo "$c $!"',
].join("\n");

const SHELL = ["/bin/bash", "/usr/bin/bash", "/usr/local/bin/bash"].find((p) => Exec("/bin/sh", ["-c", 'test -x "$1"', "t", p]).code === 0) || "/bin/sh";

const STOP = 'kill -TERM "$1" 2>/dev/null; i=0; while kill -0 "$1" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.05; i=$((i+1)); done';

const live = new Set();

export function spawnHelper(cmd, opts = {}) {
    const xo = {};
    if (opts.cwd)
        xo.cwd = opts.cwd;
    const r = Exec(SHELL, ["-c", LAUNCH, "helper", cmd, opts.pidFile || "", opts.cleanup || "", String(selfPid())], xo);
    const [pid, watcher] = String(r.stdout).trim().split(/\s+/).map((x) => parseInt(x, 10));
    if (r.code !== 0 || !(pid > 0) || !(watcher > 0))
        throw new Error("spawnHelper: cannot start: " + cmd + " (" + String(r.stderr).trim() + ")");
    let open = true;
    const h = {
        pid,
        watcher,
        cmd,
        stop() {
            if (!open)
                return;
            open = false;
            live.delete(h);
            Exec("/bin/sh", ["-c", STOP, "stop", String(watcher)]);
        },
    };
    live.add(h);
    return h;
}

export function cleanupOnExit(cmd) {
    return spawnHelper(":", { cleanup: cmd });
}

export function stopHelpers() {
    for (const h of [...live])
        h.stop();
}

export function liveHelpers() {
    return live.size;
}
