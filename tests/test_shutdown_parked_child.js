// flags: --std
import { readFileAsync, Path } from "dyna:file";
import { pipe, lines } from "dyna:stream";
import { Crawl, Fetcher, Extractor } from "dyna:scrape";
import { HTMLParse, Selector, HTMLText } from "dyna:html";
import { TCPServer, HTTPClient } from "dyna:net";
import { App } from "dyna:http";

const shape = scriptArgs[1];
const T = scriptArgs[2];

const pending = () => new Promise(() => {});
const watching = (tag, p) =>
    p.then(v => print("RESOLVED " + tag + ": " + JSON.stringify(v)),
           e => print("REJECTED " + tag + ": " + e.message));

const mockClient = () => ({
    requestAsync() { return pending(); },
    request() { throw new Error("no sync arm in this mock"); },
});

const scrapeParked = (tag) => {
    const f = new Fetcher({
        agent: "shutdown-parked/1", client: mockClient(), minDelayMs: 0,
        robots: false, allowPrivateHosts: true,
    });
    const ex = new Extractor({ title: { sel: new Selector("h1") } },
                             { text: HTMLText });
    const c = new Crawl(f, { maxPages: 2, concurrency: 2 })
        .start("http://example.invalid/" + tag, ex, HTMLParse);
    return c;
};

const pipeParked = (tag) => {
    const src = { read() { return pending(); }, close() {} };
    const dst = { write() { return pending(); }, close() {} };
    return pipe(src, dst);
};

const linesParked = (tag) => {
    const src = { read() { return pending(); }, close() {} };
    const it = lines(src);
    return it.next();
};

switch (shape) {
case "file-throw":
    watching("file", readFileAsync(new Path(T + "/fifo")));
    print("parked");
    throw new Error("exit-driver");

case "file-gc-drop-throw":
    readFileAsync(new Path(T + "/fifo"));
    if (globalThis.gc) globalThis.gc();
    print("parked-dropped");
    throw new Error("exit-driver");
    break;

case "file-async-uncaught":
    readFileAsync(new Path(T + "/fifo"));
    print("parked");
    Promise.reject(new Error("orphan-rejection"));
    break;

case "file-normal-complete":
    watching("file", readFileAsync(new Path(T + "/fifo2")));
    print("parked");
    break;

case "stream-normal":
    watching("pipe", pipeParked("s"));
    print("parked");
    break;

case "stream-lines-normal":
    watching("lines", linesParked("s"));
    print("parked");
    break;

case "scrape-normal":
    watching("crawl", scrapeParked("s").next());
    print("parked");
    break;

case "multi-throw":
    watching("file", readFileAsync(new Path(T + "/fifo")));
    watching("pipe", pipeParked("s"));
    watching("crawl", scrapeParked("s").next());
    print("parked");
    throw new Error("exit-driver");

case "multi-normal":
    watching("pipe", pipeParked("s"));
    watching("crawl", scrapeParked("s").next());
    watching("lines", linesParked("s"));
    print("parked");
    break;

case "multi-gc-drop":
    pipeParked("s");
    scrapeParked("s").next();
    linesParked("s");
    if (globalThis.gc) globalThis.gc();
    print("parked-dropped");
    break;

case "scrape-park-uncaught":
    watching("crawl", scrapeParked("s").next());
    print("parked");
    throw new Error("exit-driver");

case "http-async-inflight":
    {
        const srv = new TCPServer();
        srv.start({ connect: () => {} });
        const cl = new HTTPClient();
        watching("http", cl.postAsync("http://127.0.0.1:" + srv.port + "/", "hi"));
    }
    print("parked");
    throw new Error("exit-driver");

case "http-app-parked":
    {
        const app = new App({ port: 0 });
        app.rpc("/x", { slow: () => pending() });
        app.start();
        const cl2 = new HTTPClient();
        watching("app", cl2.postAsync("http://127.0.0.1:" + app.port + "/x",
                   JSON.stringify({ jsonrpc: "2.0", method: "slow", id: 1 })));
    }
    print("parked");
    throw new Error("exit-driver");

case "worker-then-rejection":
    {
        const os = await import("os");
        if (typeof os.Worker !== "function") {
            print("SKIP worker-unavailable");
            break;
        }
        let w;
        try {
            w = new os.Worker("tests/test_shutdown_parked_worker.js");
        } catch (e) {
            print("SKIP worker-unavailable: " + e.message);
            break;
        }
        await new Promise((res) => { w.onmessage = res; });
        await new Promise((r) => setTimeout(r, 500));
        print("worker-gone");
        Promise.reject(new Error("post-worker-rejection"));
        await new Promise((r) => setTimeout(r, 400));
        print("survived");
        break;
    }

default:
    print("unknown shape " + shape);
    throw new Error("unknown shape");
}
