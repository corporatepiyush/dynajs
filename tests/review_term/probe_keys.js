// flags: --std
/* review probe: dump N keypress() events as pipe-delimited lines.
 * driven by keys_driver.py which owns the byte timing. The READY marker is
 * flushed before the first read so the driver can synchronize: without it the
 * child's startup time races the scenario's byte timing and coalesces writes
 * that the scenario needs split. */
import { keypress } from "dyna:cli";
import * as std from "std";
const N = 40;
std.out.puts("READY\n");
std.out.flush();
for (let i = 0; i < N; i++) {
    const k = keypress();
    if (k === null) { print("EOF"); break; }
    let hex = "";
    for (const ch of k.sequence) hex += ch.codePointAt(0).toString(16) + ",";
    print("E|" + (k.name === null ? "<null>" : k.name) + "|" + hex +
          "|" + (k.ctrl ? 1 : 0) + (k.meta ? 1 : 0) + (k.shift ? 1 : 0));
}
