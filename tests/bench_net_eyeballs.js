/* bench_net_eyeballs.js — connect+close cost of the connectHappy race.
 * Sequential cycles on the shared reactor; each cycle is a full resolve +
 * race + close round trip (setup is part of what is measured). Emits
 * machine-readable ns/op lines. Run: dynajs tests/bench_net_eyeballs.js [N] */
import { TCPServer, connectHappy } from "dyna:net";

const N = parseInt(scriptArgs[1] || "200", 10);
const srv = new TCPServer({ port: 0 });
srv.start({});

function run(name, host, done) {
    let i = 0;
    const t0 = performance.now();
    (function step() {
        if (i++ >= N) {
            const ns = (performance.now() - t0) * 1e6 / N;
            print(name + " " + ns.toFixed(1) + " ns/op");
            done();
            return;
        }
        const h = connectHappy(host, srv.port, { fallbackMs: 1000 }, {
            connect: (c, err) => { if (c) c.close(); h.close(); step(); },
        });
        if (!h) { print(name + " FAILED at cycle " + i); done(); }
    })();
}

run("connectHappy_v4_connect_close", "127.0.0.1", () => {
    run("connectHappy_dual_connect_close", "localhost", () => srv.close());
});
