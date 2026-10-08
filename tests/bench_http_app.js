import { App } from "dyna:net";
import * as std from "std";

const port = parseInt(scriptArgs[1] || "8099", 10);
const uptime = parseInt(scriptArgs[2] || "30000", 10);

const app = new App({ port });

app.rpc("/rpc", {
    ping: () => "pong",
    add: ([a, b]) => a + b,
    record: ([i]) => ({
        id: i,
        name: "record-name-" + i,
        path: "/api/v1/resource/" + i,
        active: (i & 1) === 0,
        score: i * 1.5
    }),
});

app.start();
print("LISTENING " + app.port);

{
    const pf = std.getenv("DYNA_BENCH_PORT_FILE");
    if (pf) { const f = std.open(pf, "w"); f.puts(String(app.port)); f.close(); }
}

std.out.flush();

setTimeout(() => { app.close(); }, uptime);
