/* helper: start HTTPServerAsync on a fixed port for external load testing
 * (used by the docker io_uring ASan run). Not part of `make test`. */
import { HTTPServerAsync } from "dyna:net";
import * as std from "std";
const port = parseInt(scriptArgs[1] || "18099", 10);
const uptime = parseInt(scriptArgs[2] || "12000", 10);
const s = new HTTPServerAsync({ port, routes: { "/": "hello world\n" } });
s.start();
print("LISTENING " + s.port);
std.out.flush();
setTimeout(() => { s.close(); print("closed clean"); }, uptime);
