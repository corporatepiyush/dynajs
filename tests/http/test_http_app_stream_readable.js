// covers: integration c:src/modules/http/http-app-dispatch.c c:src/modules/http/http-module.c -- the rpc stream result path spans detection and dispatch
// v10b-18: an App rpc handler may return a WHATWG ReadableStream, or { stream: ReadableStream, contentType?, status?, headers? }, wherever a
// read(buf) ByteSource was accepted: the reply goes out chunked, outside the JSON-RPC envelope, read through the same C path (the stream is
// locked for the response). A stream that is already locked cannot be read, so the reply is a 500, not a hang. A batch still refuses a
// streaming result.
import { suite } from "../lib/kit.js";
import { App } from "dyna:http";

const t = suite("http_app_stream_readable");
const big = "B".repeat(200000);

t.test("rpc results that are ReadableStreams stream out chunked", async (a) => {
    const app = new App({ port: 0 });
    const locked = new ReadableStream();
    locked.getReader();
    app.rpc("/r", {
        text: () => ReadableStream.from(["alpha\n", "beta\n"]).pipeThrough(new TextEncoderStream()),
        shaped: () => ({ stream: new Blob([big]).stream(), contentType: "text/x-big", status: 201 }),
        locked: () => ({ stream: locked }),
    });
    app.start();
    const call = (body) => fetch("http://127.0.0.1:" + app.port + "/r", { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify(body) });
    try {
        let r = await call({ jsonrpc: "2.0", id: 1, method: "text" });
        a.eq(r.status, 200);
        a.eq(r.headers.get("transfer-encoding"), "chunked");
        a.eq(await r.text(), "alpha\nbeta\n");
        r = await call({ jsonrpc: "2.0", id: 2, method: "shaped" });
        a.eq(r.status, 201);
        a.eq(r.headers.get("content-type"), "text/x-big");
        a.eq((await r.text()).length, big.length);
        r = await call({ jsonrpc: "2.0", id: 3, method: "locked" });
        a.eq(r.status, 500, "locked stream");
        a.match(await r.text(), /locked/);
        r = await call([{ jsonrpc: "2.0", id: 4, method: "text" }]);
        a.match(await r.text(), /streaming body not allowed in a batch/);
    } finally {
        app.close();
    }
});

await t.run();
