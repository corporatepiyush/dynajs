// covers: integration dyna:http.HTTPServer dyna:http.HTTPServerAsync -- route-index differential that must agree on both server types
// Static route tables of 16 or more routes are looked up through a hash index built at
// construction; smaller ones keep the linear scan. The oracle is a differential: one server with
// 15 routes (scan) and one with the same 15 plus one more (index) must answer every probe the
// same way, near misses included, and every table size around the threshold must serve every
// route's own body.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_route_index");
import { HTTPServer, HTTPServerAsync, HTTPClient } from "dyna:http";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };

function routes(n) {
    const r = {};
    for (let i = 0; i < n; i++) r["/r/" + i] = { status: 200, contentType: "text/plain", body: "route-" + i };
    return r;
}

function serve(Ctor, n, extra) {
    const r = routes(n);
    if (extra) r["/extra/only"] = "extra";
    const s = new Ctor({ port: 0, routes: r });
    s.start();
    return s;
}

const c = new HTTPClient();
async function get(s, path) {
    const r = await c.get("http://127.0.0.1:" + s.port + path);
    return r.status + ":" + r.body;
}

const probes = ["/r/0", "/r/1", "/r/14", "/r/15", "/r/10", "/r/1/", "/r/01", "/R/1", "/r/1?x=1", "/r", "/r/", "/", "/extra", "/r/1%20", "/r/-1"];

await __kitS.section(": probe", async () => {
try {
    for (const Ctor of [HTTPServer, HTTPServerAsync]) {
        const scan = serve(Ctor, 15, false);
        const idx = serve(Ctor, 15, true);
        try {
            for (const p of probes)
                ok(await get(scan, p) === await get(idx, p), Ctor.name + ": probe " + p + " answers the same with and without the index (" + await get(scan, p) + " vs " + await get(idx, p) + ")");
            ok((await get(idx, "/extra/only")) === "200:extra", Ctor.name + ": the 16th route is served");
            ok((await get(scan, "/extra/only")).startsWith("404"), Ctor.name + ": and is absent from the 15-route table");
        } finally {
            scan.close();
            idx.close();
        }
        for (const n of [16, 17, 64, 300]) {
            const s = serve(Ctor, n, false);
            try {
                let bad = 0;
                for (let i = 0; i < n; i++)
                    if (await get(s, "/r/" + i) !== "200:route-" + i) bad++;
                ok(bad === 0, Ctor.name + ": all " + n + " routes serve their own body (" + bad + " wrong)");
                ok((await get(s, "/r/" + n)).startsWith("404"), Ctor.name + ": one past the table is 404 at " + n);
                ok((await get(s, "/r/" + (n - 1) + "/")).startsWith("404"), Ctor.name + ": a trailing slash is a different path at " + n);
            } finally {
                s.close();
            }
        }
    }
} finally {
    c.close();
}
});
__kitS.end();

print(`test_http_route_index: ${pass} passed, ${fail} failed`);
if (fail) throw new Error("test_http_route_index failed");
