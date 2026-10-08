// 05 · WebSocket chat room — broadcast server with an origin check, plus scripted clients.
//
// WHAT IT SHOWS
//   - App.ws: upgrade (the place to authenticate), open, message and close handlers
//   - checking Origin before the 101, which is what stops cross-site WebSocket hijacking
//   - WsClient: the same protocol from the client side
//
// RUN      dynajs examples/apps/05-websocket-chat.js
// DEPLOY   PORT=8080 ALLOWED_ORIGIN=https://chat.example dynajs examples/apps/05-websocket-chat.js

import { App, WsClient } from "dyna:net";
import { getEnv } from "dyna:sys";

const PORT = Number(getEnv("PORT") ?? 0);
const ALLOWED_ORIGIN = getEnv("ALLOWED_ORIGIN");   // unset = any origin (development)
const MAX_MESSAGE = 2000;

const members = new Map();                        // socket -> display name
let nextGuest = 1;

function broadcast(payload, except) {
    const text = JSON.stringify(payload);
    for (const socket of members.keys()) if (socket !== except) socket.send(text);
}

const app = new App({ port: PORT });
app.ws("/chat", {
    // Runs BEFORE the handshake completes. Browsers attach cookies to
    // cross-site WebSocket requests, so a cookie-authenticated endpoint must
    // check the Origin here. A falsy return answers 403.
    upgrade: (req) => !ALLOWED_ORIGIN || req.origin === ALLOWED_ORIGIN,
    open: (socket) => {
        const name = "guest" + nextGuest++;
        members.set(socket, name);
        socket.send(JSON.stringify({ type: "welcome", name, online: members.size }));
        broadcast({ type: "joined", name }, socket);
    },
    message: (socket, data, isBinary) => {
        if (isBinary || data.length > MAX_MESSAGE) return socket.close();   // not a chat message
        broadcast({ type: "message", from: members.get(socket), text: String(data) });
    },
    close: (socket) => {
        const name = members.get(socket);
        members.delete(socket);
        broadcast({ type: "left", name });
    },
});
app.get("/online", () => ({ online: members.size }));
app.start();
console.log("chat server on port", app.port);

// ---- self-test: two clients talk to each other -----------------------------
if (!PORT) {
    const url = `ws://127.0.0.1:${app.port}/chat`;
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };

    // A tiny client wrapper that records everything it receives and lets the
    // test await "the next message of this type".
    function client() {
        const inbox = [];
        const waiters = [];
        const ws = new WsClient(url, {
            message: (_ws, data) => {
                const msg = JSON.parse(String(data));
                inbox.push(msg);
                for (let i = waiters.length - 1; i >= 0; i--)
                    if (waiters[i].type === msg.type) waiters.splice(i, 1)[0].resolve(msg);
            },
        });
        const next = (type) => {
            const have = inbox.find((m) => m.type === type && !m.taken);
            if (have) { have.taken = true; return Promise.resolve(have); }
            return new Promise((resolve) => waiters.push({ type, resolve })).then((m) => (m.taken = true, m));
        };
        return { ws, inbox, next };
    }

    const alice = client();
    const hello = await alice.next("welcome");
    check(hello.name === "guest1" && hello.online === 1, "the first client is welcomed");

    const bob = client();
    await bob.next("welcome");
    check((await alice.next("joined")).name === "guest2", "existing members hear about a join");

    alice.ws.send("hello bob");
    const heard = await bob.next("message");
    check(heard.from === "guest1" && heard.text === "hello bob", "a message reaches the other member");

    bob.ws.close();
    check((await alice.next("left")).name === "guest2", "a departure is announced");
    alice.ws.close();
    await sleep(50);                               // let the close frames land
    check(members.size === 0, "the room is empty again");
    console.log("self-test passed: join, broadcast, leave");
    app.close();
}
