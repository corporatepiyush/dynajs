// 06 · Live event stream — pushes job progress to browsers over Server-Sent Events.
//
// WHAT IT SHOWS
//   - App.sse: a long-lived one-way stream, the simplest way to push to a browser
//   - fan-out to every connected listener, with cleanup when one disconnects
//   - reading an SSE stream by hand over a raw TCP connection (what EventSource does)
//
// RUN      dynajs examples/apps/06-server-sent-events.js
// DEPLOY   PORT=8080 dynajs examples/apps/06-server-sent-events.js
//          then in a browser: new EventSource("/events").onmessage = (e) => console.log(e.data)

import { App, TCPServer } from "dyna:net";
import { getEnv } from "dyna:sys";

const PORT = Number(getEnv("PORT") ?? 0);
const listeners = new Set();

// publish() is what the rest of your program calls; it does not care who listens.
function publish(event) {
    const data = JSON.stringify(event);
    for (const stream of listeners) stream.send(data);
}

const app = new App({ port: PORT });
app.sse("/events", {
    open: (stream) => {
        listeners.add(stream);
        stream.send(JSON.stringify({ type: "hello", listeners: listeners.size }));
    },
    close: (stream) => listeners.delete(stream),
});

// Starting a job answers immediately; progress arrives on the event stream.
app.post("/jobs", (req) => {
    const { name, steps } = JSON.parse(req.body);
    (async () => {
        for (let i = 1; i <= steps; i++) {
            await sleep(10);                        // stands in for real work
            publish({ type: "progress", job: name, done: i, of: steps });
        }
        publish({ type: "finished", job: name });
    })();
    return { status: 202, body: "started" };
});
app.start();
console.log("event stream on port", app.port);

// ---- self-test -------------------------------------------------------------
if (!PORT) {
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
    const events = [];
    let raw = "";
    let finished;
    const done = new Promise((resolve) => (finished = resolve));

    // An SSE response never ends, so read it as a stream: connect, send the
    // request line, and split the body on blank lines as EventSource would.
    const conn = TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
        connect: (c, err) => {
            if (err) throw new Error(err);
            c.write("GET /events HTTP/1.1\r\nHost: localhost\r\nAccept: text/event-stream\r\n\r\n");
        },
        data: (_c, bytes) => {
            raw += new TextDecoder().decode(bytes);
            let cut;
            while ((cut = raw.indexOf("\n\n")) >= 0) {
                const block = raw.slice(0, cut);
                raw = raw.slice(cut + 2);
                for (const line of block.split("\n"))
                    if (line.startsWith("data:")) {
                        const ev = JSON.parse(line.slice(5).trim());
                        events.push(ev);
                        if (ev.type === "finished") finished();
                    }
            }
        },
    });

    while (listeners.size === 0) await sleep(5);     // wait for the subscription
    const res = await fetch(`http://127.0.0.1:${app.port}/jobs`,
                            { method: "POST", body: JSON.stringify({ name: "export", steps: 3 }) });
    check(res.status === 202, "the job is accepted at once");
    await done;

    const kinds = events.map((e) => e.type).join(",");
    check(kinds === "hello,progress,progress,progress,finished", "events arrive in order: " + kinds);
    check(events[3].done === 3 && events[3].of === 3, "progress carries its counters");
    conn.close();
    await sleep(30);
    check(listeners.size === 0, "the listener is removed when the client goes away");
    console.log("self-test passed:", events.length, "events streamed");
    app.close();
}
