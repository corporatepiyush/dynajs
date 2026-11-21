// flags: --std
// The server side of test_web_streams_bridge.js's getStream case, on another thread (it predates T1, when HTTPClient blocked its thread; the case keeps exercising a server off the client thread).
import * as os from "os";
import { TCPServer } from "dyna:net";
const parent = os.Worker.parent;
const srv = new TCPServer({ port: 0 });
srv.start({
    data: (c) => {
        let body = "";
        for (let i = 0; i < 2000; i++) body += "row " + i + "\n";
        const head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
        const half = body.length >> 1;
        const chunk = (s) => s.length.toString(16) + "\r\n" + s + "\r\n";
        c.write(head + chunk(body.slice(0, half)) + chunk(body.slice(half)) + "0\r\n\r\n");
    },
});
parent.onmessage = (e) => { if (e.data === "stop") { srv.close(); parent.onmessage = null; } };
parent.postMessage({ port: srv.port });
