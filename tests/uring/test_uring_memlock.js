// flags: --std
// covers: c:src/dyna-aio-uring.c
// A reactor must still be creatable under an ordinary RLIMIT_MEMLOCK. io_uring
// ring memory is charged against it and a closed ring's charge is returned
// asynchronously, so a fixed 8192-entry ring (~0.8 MiB) failed with ENOMEM
// after about ten reactors -- or ten quick open/close cycles -- at the common
// 8 MiB limit. The LIMIT is the test: the child runs under `ulimit -l 8192`.
// On kqueue/epoll builds nothing is charged and this passes trivially.
import * as std from "std";
import * as os from "os";
// The child gets its program with -e and reports on stdout: as uid 65534 it cannot read anything
// the test runner created (its per-worker temp dirs are 0700 root; the gate's linux leg failed
// "child.js: Permission denied" -> "only -1 of 40"), so it touches no file at all.
const CHILD = `(async () => {
    const { Spawn } = await import("dyna:sys");
    const { TCPServer } = await import("dyna:net");
    let made = 0, why = "";
    try {
        for (let i = 0; i < 40; i++) {
            const s = new TCPServer({ port: 0 }, { data() {} });
            s.close();
            const c = new Spawn("/bin/sh", ["-c", "exit 0"]);
            await c.wait();
            c.close();
            made++;
        }
    } catch (e) { why = String(e && e.message); }
    std.out.puts("RESULT " + JSON.stringify({ made, why }) + "\\n");
    std.out.flush();
})();`;
// io_uring charges ring memory to the USER (user->locked_vm, summed over every process of that uid)
// and compares the sum with the caller's own limit. Run as root beside other ring holders (the gate
// runs suites 8 at a time, and run_leg lifts their memlock limit), this child's 8 MiB was spent
// by its neighbours ("only 1 of 40" in the linux uring leg). So when root and setpriv are available
// the child runs as uid/gid 65534, whose budget nothing else touches; otherwise as before.
const q = (x) => "'" + x.replace(/'/g, "'\\''") + "'";
const [r, w] = os.pipe();
os.exec(["/bin/sh", "-c", "ulimit -l 8192 2>/dev/null; C=" + q("exec ./dynajs --std -e " + q(CHILD)) + "; " +
    "if [ \"$(id -u)\" = 0 ] && command -v setpriv >/dev/null 2>&1; then exec setpriv --reuid=65534 --regid=65534 --clear-groups sh -c \"$C\"; else eval \"$C\"; fi"],
    { stdout: w });
os.close(w);
const f = std.fdopen(r, "r");
const out = f.readAsString();
f.close();
let R = { made: -1, why: "the child printed no result: " + JSON.stringify(out.slice(0, 300)) };
const m = /^RESULT (.*)$/m.exec(out);
if (m) R = JSON.parse(m[1]);
if (R.made !== 40)
    throw new Error("test_uring_memlock: only " + R.made + " of 40 reactor open/close cycles under an 8 MiB memlock limit (" + R.why + ")");
print("test_uring_memlock: 40 reactor open/close cycles under an 8 MiB memlock limit");
