// flags: --std
import "./httpc.js";
import { pipe } from "dyna:stream";
import { Crawl, Fetcher, Extractor } from "dyna:scrape";
import { Selector, HTMLText, HTMLParse } from "dyna:html";

const shape = scriptArgs[1];
const T = scriptArgs[2];
const pending = () => new Promise(() => {});

switch (shape) {

case "template-t1": {
    const sweep = await import("./park_sweep.so");
    const p = sweep.park();
    p.then(v => print("RESOLVED " + v),
           e => print("REJECTED " + e.message));
    sweep.settle();
    print("settled");
    break;
}

case "template-t2": {
    const sweep = await import("./park_sweep.so");
    const p = sweep.park();
    p.then(v => print("RESOLVED " + v),
           e => print("REJECTED " + e.message));
    print("parked");
    break;
}

case "template-t3": {
    const sweep = await import("./park_sweep.so");
    sweep.park().then(v => print("RESOLVED " + v), e => {
        print("REJECTED 1 " + e.message);
        sweep.park().then(v2 => print("RESOLVED 2"),
                          e2 => print("REJECTED 2 " + e2.message));
    });
    print("parked");
    break;
}

case "repark-pipe": {
    const mkPipe = (tag) => pipe({ read() { return pending(); }, close() {} },
                                 { write() { return pending(); }, close() {} });
    mkPipe("1").then(v => print("RESOLVED 1"),
      e => {
        print("REJECTED 1: " + e.message);
        mkPipe("2").then(v2 => print("RESOLVED 2"),
                         e2 => print("REJECTED 2: " + e2.message));
      });
    break;
}

case "drain-throw": {
    pipe({ read() { return pending(); }, close() {} },
         { write() { return pending(); }, close() {} }).then(
        v => print("RESOLVED"),
        e => {
            print("REJECTED: " + e.message);
            queueMicrotask(() => {
                print("microtask-ran");
                throw new Error("drain-throw-probe");
            });
        });
    print("parked");
    break;
}

case "drain-nothrow": {
    pipe({ read() { return pending(); }, close() {} },
         { write() { return pending(); }, close() {} }).then(
        v => print("RESOLVED"),
        e => print("REJECTED: " + e.message));
    print("parked");
    break;
}

case "selfreg-sweep": {
    const sweep = await import("./park_sweep.so");
    sweep.parkRereg().then(v => print("RESOLVED " + v),
                           e => print("REJECTED: " + e.message));
    print("parked-selfreg");
    break;
}

case "repark-waiter": {
    const client = {
        requestAsync(method, url) {
            client.n++;
            if (client.n === 1) {
                return Promise.resolve({
                    status: 200, url, headers: {},
                    body: "<html><body><h1>t</h1>" +
                        '<a href="http://example.invalid/b">x</a>' +
                        '<a href="http://example.invalid/c">y</a>' +
                        '<a href="http://example.invalid/d">z</a>' +
                        "</body></html>",
                });
            }
            return pending();
        },
        request() { throw new Error("no sync arm"); },
    };
    client.n = 0;
    const f = new Fetcher({
        agent: "repark-waiter/1", client, minDelayMs: 0,
        robots: false, allowPrivateHosts: true,
    });
    const ex = new Extractor({
        links: { sel: new Selector("a"), attr: "href", all: true },
    }, { text: HTMLText });
    const c = new Crawl(f, { maxPages: 10, concurrency: 2 })
        .start("http://example.invalid/a", ex, HTMLParse);

    const first = await c.next();
    print("page1 " + (first && first.value && first.value.url));
    c.next().then(v => print("page2 " + (v && v.value && v.value.url)),
      e => {
        print("REJECTED outer: " + e.message);
        c.next().then(v2 => print("page3 " + (v2 && v2.value && v2.value.url)),
                      e2 => print("REJECTED inner: " + e2.message));
      });
    break;
}

case "graveyard-settle": {
    let res1 = null;
    const client = {
        requestAsync() {
            return new Promise(r => { res1 = r; });
        },
        request() { throw new Error("no sync arm"); },
    };
    const f = new Fetcher({
        agent: "graveyard-settle/1", client, minDelayMs: 0,
        robots: false, allowPrivateHosts: true,
    });
    const ex = new Extractor({ title: { sel: new Selector("h1") } },
                             { text: HTMLText });
    const c = new Crawl(f, { maxPages: 2, concurrency: 2 })
        .start("http://example.invalid/a", ex, HTMLParse);
    c.next().then(v => print("page " + (v && v.value && v.value.url)),
      e => {
        print("REJECTED: " + e.message);
        if (res1) {
            res1({ status: 200, url: "http://example.invalid/a",
                   headers: {},
                   body: "<html><body><h1>t</h1></body></html>" });
            res1({ status: 200, url: "http://example.invalid/x",
                   headers: {},
                   body: "<html><body><h1>t2</h1></body></html>" });
            print("stale-settle-landed");
        } else {
            print("no parked promise captured");
        }
      });
    break;
}

default:
    print("unknown shape " + shape);
    throw new Error("unknown shape");
}
