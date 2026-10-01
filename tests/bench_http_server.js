import { HTTPServer } from "dyna:net";
import * as std from "std";

const port = parseInt(scriptArgs[1] || "8098", 10);
const workers = parseInt(scriptArgs[2] || "4", 10);
const uptime = parseInt(scriptArgs[3] || "30000", 10);

const server = new HTTPServer({
    port,
    workers,
    routes: {
        "/": "hello world\n",
        "/json": { status: 200, contentType: "application/json",
                   body: JSON.stringify({ msg: "ok", engine: "dynajs" }) },
    },
});
server.start();
print("LISTENING " + server.port);

{
    const pf = std.getenv("DYNA_BENCH_PORT_FILE");
    if (pf) { const f = std.open(pf, "w"); f.puts(String(server.port)); f.close(); }
}

std.out.flush();

setTimeout(() => { server.close(); }, uptime);
