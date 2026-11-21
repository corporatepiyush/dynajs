// covers: dyna:http.HTTPServerAsync
// Static route tables of 16 or more paths are looked up through a hash index
// (fewer use a linear scan). The index used to be the SwissMap core map; with
// SwissMap removed it is a u32 open-addressing table over the route array
// (slot = route index + 1, a 32-bit hash tag beside it, the path compared in
// place). Pinned here: every path of a 15-route (scan) and a 300-route (index)
// server answers with its own body, and near-miss and unknown paths are 404.
import { suite } from "../lib/kit.js";
import { HTTPServerAsync } from "dyna:http";

const t = suite("net_http_server_async_route_index");

function serve(n) {
    const routes = {};
    for (let i = 0; i < n; i++) routes["/r/" + i] = { status: 200, contentType: "text/plain", body: "b" + i };
    const s = new HTTPServerAsync({ port: 0, routes });
    s.start();
    return s;
}

for (const n of [15, 300]) {
    t.test(n + " routes: each path serves its own body, misses are 404", async (a) => {
        const s = serve(n);
        try {
            const B = "http://127.0.0.1:" + s.port;
            let bad = 0;
            for (let i = 0; i < n; i++) {
                const r = await fetch(B + "/r/" + i);
                if (r.status !== 200 || (await r.text()) !== "b" + i) bad++;
            }
            a.eq(bad, 0, "every registered path answers with its body");
            for (const p of ["/r/" + n, "/r/", "/r", "/r/1/", "/r/01", "/R/1", "/zz"]) {
                const r = await fetch(B + p);
                await r.text();
                a.eq(r.status, 404, "miss " + p);
            }
        } finally { s.close(); }
    });
}

await t.run();
