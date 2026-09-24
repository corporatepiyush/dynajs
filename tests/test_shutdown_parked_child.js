// flags: --std
/* test_shutdown_parked_child.js -- the exit SHAPE driver.
 *
 * One shape per invocation (argv[1]), driven by test_shutdown_parked.js,
 * which judges this process's exit code and output. A shape parks native
 * work across JS (a FIFO read with no writer, a pipeline over a source that
 * never resolves, a crawl flight on a requestAsync that never settles) and
 * then leaves through the exit path its name describes. The engine's
 * contract: the parked promise FAILS with a rejection naming engine
 * shutdown (observable to a .catch handler while JS is still legal), the
 * teardown asserts nothing, and the exit code is the shape's own -- a
 * clean 0 for a normal end, the error's 1 for a thrown one.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_shutdown_parked_child.js
 *      <shape> <tmpdir> */
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
    /* the named repro: readFileAsync parked on a FIFO with no writer and an
       uncaught throw -> used to abort 134 in JS_FreeRuntime */
    watching("file", readFileAsync(new Path(T + "/fifo")));
    print("parked");
    throw new Error("exit-driver");

case "file-gc-drop-throw":
    readFileAsync(new Path(T + "/fifo")); /* dropped: nobody holds it */
    if (globalThis.gc) globalThis.gc();
    print("parked-dropped");
    throw new Error("exit-driver");
    break;

case "file-async-uncaught":
    /* parked read + a rejection nobody handles: the unhandled-rejection
       check ends the process (exit 1) while the read is still parked */
    readFileAsync(new Path(T + "/fifo"));
    print("parked");
    Promise.reject(new Error("orphan-rejection"));
    break;

case "file-normal-complete":
    /* CONTROL: the writer comes, the parked read RESOLVES normally -- the
       shutdown machinery must not touch in-flight work */
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
    /* parked crawl + uncaught throw, the rejection observed beside it */
    watching("crawl", scrapeParked("s").next());
    print("parked");
    throw new Error("exit-driver");

case "http-async-inflight":
    /* t3-x1 ITEM 3: an HTTPClient exchange still in flight (a blackhole peer
       answers nothing) with an uncaught throw. The wrapper's self-pin is a
       C-held JSValue the collector deliberately cannot see, so without the
       per-job shutdown sweep the wrapper is never collectable and
       JS_FreeRuntime aborts on gc_obj_list (exit 134). The sweep releases
       the pin and settles the park while JS is still legal. */
    {
        const srv = new TCPServer();
        srv.start({ connect: () => {} });
        const cl = new HTTPClient();
        watching("http", cl.postAsync("http://127.0.0.1:" + srv.port + "/", "hi"));
    }
    print("parked");
    throw new Error("exit-driver");

case "http-app-parked":
    /* the App.rpc spot check, made permanent: a handler returns a
       never-settling promise, a client of the app is waiting, and an
       uncaught throw leaves through the top. The waiting client is itself a
       parked exchange (same sweep), so its rejection must surface, and the
       conn husk the pend holds must tear down without an abort. */
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
    /* A WORKER runtime finishes first (its js_std_free_handlers runs while
       this, the parent, runtime is still alive), then THIS runtime rejects
       with nobody handling it. The rejection-quieting latch is per thread,
       so the report must still happen: exit 1 with the reason on stderr.
       A process-wide latch would silence the tracker here and the process
       would exit 0 after "survived" (the mutation this row exists to
       catch). Skips (rc 0) where os.Worker is unavailable -- the parent
       row turns that into a failure under DYNAJS_REQUIRE_TOOLS=1. */
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
        /* let the worker's runtime finish its own teardown (free_handlers) */
        await new Promise((r) => setTimeout(r, 500));
        print("worker-gone");
        Promise.reject(new Error("post-worker-rejection"));
        await new Promise((r) => setTimeout(r, 400));
        print("survived");         /* only when the report was swallowed */
        break;
    }

default:
    print("unknown shape " + shape);
    throw new Error("unknown shape");
}
