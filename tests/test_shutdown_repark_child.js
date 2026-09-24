// flags: --std
/* test_shutdown_repark_child.js -- the shutdown re-park / template shapes.
 *
 * One shape per invocation (argv[1]), driven by test_shutdown_repark.js,
 * which judges this process's exit code and output. Each shape is a
 * permanent row of a review probe: a park made DURING the shutdown drain
 * (a .catch that retries), the net-style park template's normal and
 * shutdown settle paths, a next() re-parked on a dead crawl, a stale
 * settle landing on a graveyarded flight, a drain whose user code throws
 * (with its no-throw control), and a sweep that re-registers itself (the
 * bounded walk).
 *
 * The contract they pin: a drain-time re-park is a STRAGGLER -- released
 * WITHOUT settling (the inner .catch never fires) and its pins collected
 * before the gc_obj_list assertion, so every shape exits with its own
 * code and no abort. The shapes that reach teardown WITHOUT an exit
 * driver exist because their parks have no completer in the JS heap; the
 * file re-park shape carries a throw because a parked FIFO read holds the
 * event loop until the kernel answers (the documented liveness policy --
 * see quiet-file and dyna-file.c).
 *
 * Run: dynajs tests/test_shutdown_repark_child.js <shape> <tmpdir> */
import { readFileAsync, writeFile, Path } from "dyna:file";
import { pipe } from "dyna:stream";
import { Crawl, Fetcher, Extractor } from "dyna:scrape";
import { Selector, HTMLText, HTMLParse } from "dyna:html";

const shape = scriptArgs[1];
const T = scriptArgs[2];
const pending = () => new Promise(() => {});

switch (shape) {

case "template-t1": {
    /* the normal settle path: remove-after-settle, exactly-once free.
       Exit 0, value 42. */
    const sweep = await import("./park_sweep.so");
    const p = sweep.park();
    p.then(v => print("RESOLVED " + v),
           e => print("REJECTED " + e.message));
    sweep.settle();
    print("settled");
    break;
}

case "template-t2": {
    /* a net-style park with no completion: the shutdown sweep must FAIL
       it observably at teardown and the process must exit 0. */
    const sweep = await import("./park_sweep.so");
    const p = sweep.park();
    p.then(v => print("RESOLVED " + v),
           e => print("REJECTED " + e.message));
    print("parked");
    break;
}

case "template-t3": {
    /* re-park inside the drain's .catch (the straggler path): the second
       park is released WITHOUT settling -- "REJECTED 2" must never print. */
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
    /* a PIPE park added during the drain must be released WITHOUT settling
       (pipe_op_sweep is re-registered per pipe): "REJECTED 1" prints (the
       sweep's failure), "REJECTED 2" never does. */
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

case "repark-file-driver": {
    /* a FILE park added during the drain (probe_repark_file with an exit
       driver: a parked FIFO read holds the loop, so the quiet shape never
       reaches teardown -- the driver throw is what ends the process, exit
       1). The straggler must release the SECOND park's pins WITHOUT
       settling: "REJECTED 2" must never print. */
    readFileAsync(new Path(T + "/fifo")).then(
        () => print("RESOLVED 1"),
        e => {
            print("REJECTED 1: " + e.message);
            /* re-park during the drain */
            readFileAsync(new Path(T + "/fifo2")).then(
                () => print("RESOLVED 2"),
                e2 => print("REJECTED 2: " + e2.message));
        });
    print("parked");
    throw new Error("exit-driver");
}

case "drain-throw": {
    /* a park whose drain-time .catch queues a microtask that THROWS: the
       drain stops on the exception (JS_ExecutePendingJob returns -1) and
       the engine must release the unconsumed exception value exactly once
       -- leaving it in rt->current_exception used to pin the whole context
       graph through the realm ref C functions hold and abort the teardown
       on the gc_obj_list assertion (exit 134). The parked failure itself
       must still be reported: "REJECTED" prints from the drain. */
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
    /* the control for drain-throw: the same park, the same drain-time
       .catch, NO throwing microtask. The process must behave identically
       from the row's point of view (rc 0, failure reported), which is what
       makes the throwing microtask the only delta between the two rows. */
    pipe({ read() { return pending(); }, close() {} },
         { write() { return pending(); }, close() {} }).then(
        v => print("RESOLVED"),
        e => print("REJECTED: " + e.message));
    print("parked");
    break;
}

case "selfreg-sweep": {
    /* a park whose sweep re-registers the identical (func, opaque) pair on
       every call: the pass must terminate at its documented cap (and drop
       the entry without calling it), never spin. The park fails on the
       sweep's first call, so the drain still runs and the failure is
       observable. */
    const sweep = await import("./park_sweep.so");
    sweep.parkRereg().then(v => print("RESOLVED " + v),
                           e => print("REJECTED: " + e.message));
    print("parked-selfreg");
    break;
}

case "repark-waiter": {
    /* a park ADDED during the drain on a DEAD crawl with a live queue:
       cr_dispatch is gated off and cr_sweep already ran, so parking a
       waiter would pin resolve/reject nothing releases (exit 134). The
       liveness guard must REJECT cleanly instead. */
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
            return pending();          /* never settles */
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
    /* second next(): flights /b + /c dispatch (never settle), /d stays
       QUEUED, the waiter parks -- the park the shutdown sweep must fail */
    c.next().then(v => print("page2 " + (v && v.value && v.value.url)),
      e => {
        print("REJECTED outer: " + e.message);
        /* RE-PARK DURING THE DRAIN: next() on the dead crawl */
        c.next().then(v2 => print("page3 " + (v2 && v2.value && v2.value.url)),
                      e2 => print("REJECTED inner: " + e2.message));
      });
    break;
}

case "graveyard-settle": {
    /* a stale settle landing on a GRAVEYARD flight while the drain runs:
       cr_sweep failed the parked next() and defer-freed the flight shell;
       the .catch then RESOLVES the mocked requestAsync promise, so
       cr_flight_settle fires on the graveyard shell and must take the
       dead-flag path -- reading live shell memory, never freed memory. */
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
        /* the flight shell is graveyarded NOW: land its stale settle */
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

case "quiet-file": {
    /* the LIVENESS POLICY row: a parked FIFO read's completion is an event
       source outside the JS heap (the worker + kernel), so the park keeps
       the process alive -- the judge expects this shape to still be
       running when its timeout fires (and the file-normal-complete row of
       test_shutdown_parked.js proves the other half: when the writer
       comes, the read settles from the WORK, never from the sweep). The
       marker FILE records the park: stdout is lost when the timeout kills
       the parked process. */
    writeFile(new Path(T + "/quiet.marker"), "parked-quiet");
    readFileAsync(new Path(T + "/fifo")).then(
        () => print("RESOLVED 1"),
        e => print("REJECTED 1: " + e.message));
    print("parked-quiet");
    break;
}

default:
    print("unknown shape " + shape);
    throw new Error("unknown shape");
}
