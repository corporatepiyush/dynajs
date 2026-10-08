// flags: --std
// A TCPServer.connect client is owned by its in-flight work, not by the
// caller's reference: a dropped handle survives a collection while it is
// connecting or connected, and is released once its connection ends.
import { suite } from "./kit.js";
import { TCPServer } from "dyna:net";

const t = suite("net_client_lifetime");
const gc = () => { globalThis.std.gc(); globalThis.std.gc(); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function echoServer() {
    const srv = new TCPServer({ port: 0 });
    srv.start({ data(conn, d) { conn.write(d); } });
    return srv;
}

t.test("a dropped handle survives a collection while connecting", async (a) => {
    const srv = echoServer();
    let fired = 0, err = null;
    TCPServer.connect({ host: "127.0.0.1", port: srv.port }, { connect(conn, e) { fired++; err = e; if (conn) conn.close(); } });
    gc();
    await sleep(150);
    a.eq(fired, 1, "the connect handler ran");
    a.eq(err, null);
    srv.close();
});

t.test("a dropped handle survives collections while connected", async (a) => {
    const srv = echoServer();
    let echoed = 0, closes = 0, writeErr = null;
    TCPServer.connect({ host: "127.0.0.1", port: srv.port }, {
        connect(conn, e) {
            let n = 0;
            const tick = () => {
                gc();
                try { conn.write("x"); } catch (ex) { writeErr = String(ex); return; }
                if (++n < 6) setTimeout(tick, 15);
            };
            setTimeout(tick, 15);
        },
        data(conn, d) { echoed += d.length; },
        close() { closes++; },
    });
    gc();
    await sleep(350);
    a.eq(writeErr, null, "no write hit a collected connection");
    a.eq(echoed, 6, "every byte came back");
    a.eq(closes, 0, "still open");
    srv.close();
    await sleep(100);
    a.eq(closes, 1, "the close handler ran when the peer went away");
});

t.test("several dropped clients each keep their own connection", async (a) => {
    const srv = echoServer();
    const got = [0, 0, 0, 0];
    for (let i = 0; i < 4; i++)
        TCPServer.connect({ host: "127.0.0.1", port: srv.port }, {
            connect(conn) { setTimeout(() => { gc(); conn.write("ab"); }, 20 + 10 * i); },
            data(conn, d) { got[i] += d.length; },
        });
    gc();
    await sleep(250);
    a.deep(got, [2, 2, 2, 2]);
    srv.close();
    await sleep(50);
});

t.test("an explicit close still tears a connected client down", async (a) => {
    const srv = echoServer();
    let closes = 0;
    const h = TCPServer.connect({ host: "127.0.0.1", port: srv.port }, { close() { closes++; } });
    await sleep(80);
    h.close();
    a.eq(h.closed, true);
    gc();
    await sleep(50);
    srv.close();
});

t.test("a refused connect reports and releases", async (a) => {
    let err = null;
    TCPServer.connect({ host: "127.0.0.1", port: 1 }, { connect(conn, e) { err = e; } });
    gc();
    await sleep(200);
    a.ok(typeof err === "string" && err.length > 0, "the error reached the handler");
});

await t.run();
