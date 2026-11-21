import * as sys from "dyna:sys";
import * as os from "os";
import * as std from "std";

const RX = /^\s*(\d+)\s+\S*\bbash\s+\S*(manual-run\/run2\.sh|scratchpad\/merge-[0-9]+\.sh)(\s|$)/;
const argv = sys.args().slice(1);
const opt = (name, def) => { const i = argv.indexOf(name); return i >= 0 && i + 1 < argv.length ? Number(argv[i + 1]) : def; };
const maxSec = opt("--max", 900), every = opt("--interval", 10), checkOnly = argv.includes("--check");
const busy = () => String(sys.Exec("/bin/ps", ["-axo", "pid=,args="]).stdout).split("\n").filter((l) => RX.test(l)).map((l) => l.trim().slice(0, 160));
const t0 = Date.now();
let list = busy();
while (list.length && !checkOnly && (Date.now() - t0) / 1000 < maxSec) {
    os.sleep(every * 1000);
    list = busy();
}
const waited = Math.round((Date.now() - t0) / 1000);
for (const l of list) console.log("busy-wait: running " + l);
console.log(`busy-wait: ${list.length ? "still busy" : "quiet"} after ${waited}s (runs of manual-run/run2.sh or scratchpad/merge-N.sh)`);
std.exit(list.length ? 1 : 0);
