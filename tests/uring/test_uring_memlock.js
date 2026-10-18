// flags: --std
// A reactor must still be creatable under an ordinary RLIMIT_MEMLOCK. io_uring
// ring memory is charged against it and a closed ring's charge is returned
// asynchronously, so a fixed 8192-entry ring (~0.8 MiB) failed with ENOMEM
// after about ten reactors -- or ten quick open/close cycles -- at the common
// 8 MiB limit. The LIMIT is the test: the child runs under `ulimit -l 8192`.
// On kqueue/epoll builds nothing is charged and this passes trivially.
import * as std from "std";
import * as os from "os";
import { Path, makeTempDir, removeAll, writeFile, readFile, exists } from "dyna:file";

const T = String(makeTempDir("memlock"));
writeFile(new Path(T + "/child.js"), `
import { Spawn } from "dyna:sys";
import { TCPServer } from "dyna:net";
import * as std from "std";
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
const f = std.open("${T}/out.part", "w"); f.puts(JSON.stringify({ made, why })); f.close();
import * as os from "os";
os.rename("${T}/out.part", "${T}/out");
`);
os.exec(["/bin/sh", "-c", "ulimit -l 8192 2>/dev/null; exec ./dynajs --std " + T + "/child.js"]);
let R = { made: -1, why: "the child wrote no result" };
try { R = JSON.parse(readFile(new Path(T + "/out"))); } catch (e) { }
removeAll(new Path(T));
if (R.made !== 40)
    throw new Error("test_uring_memlock: only " + R.made + " of 40 reactor open/close cycles under an 8 MiB memlock limit (" + R.why + ")");
print("test_uring_memlock: 40 reactor open/close cycles under an 8 MiB memlock limit");
