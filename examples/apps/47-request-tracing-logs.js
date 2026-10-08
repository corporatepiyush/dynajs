// 47 · Request tracing and structured logs — one correlation id follows a request through every log line.
//
// WHAT IT SHOWS
//   - dyna:log Logger: JSON lines, child loggers that add fields, levels, a rolling file
//   - a request id minted (or accepted) at the edge and attached to everything logged for that request
//   - logging outcome and duration once per request, in a shape a log pipeline can aggregate
//   - why structured logging is safe: a user-supplied value cannot forge a second log line
//   - redacting sensitive fields before they reach the log
//
// RUN      dynajs examples/apps/47-request-tracing-logs.js
// DEPLOY   PORT=8080 LOG_FILE=/var/log/app/app.log dynajs examples/apps/47-request-tracing-logs.js

import { App } from "dyna:net";
import { Logger } from "dyna:log";
import { v7, validate as isUuid } from "dyna:uuid";
import { getEnv } from "dyna:sys";
import { makeTempDir, readFile, realPath, removeAll } from "dyna:file";

const PORT = Number(getEnv("PORT") ?? 0);
const selfTest = !PORT;
const dir = selfTest ? makeTempDir("logs") : null;
const logFile = selfTest ? String(dir.join("app.log")) : getEnv("LOG_FILE");

// One root logger for the process. With rollover the data lands in numbered
// files (app.1.log, app.2.log, ...); `symlink` keeps app.log pointing at the
// active one, which is the name `tail -f` and log shippers want.
const root = new Logger({
    name: "orders-api",
    level: getEnv("LOG_LEVEL") ?? "info",
    dest: logFile,                                   // undefined = stderr
    timestamp: "iso",
    base: { service: "orders-api", version: "1.4.2" },
    rollover: logFile ? { size: "10m", count: 5, symlink: true } : undefined,
});

const SENSITIVE = /^(authorization|cookie|x-api-key|password|card)$/i;
const redact = (obj) => Object.fromEntries(Object.entries(obj).map(([k, v]) => [k, SENSITIVE.test(k) ? "<redacted>" : v]));

const json = (status, value) => ({ status, contentType: "application/json", body: JSON.stringify(value) });
const app = new App({ port: PORT });

// Middleware: give every request an id and a logger that carries it.
app.use((req) => {
    // Accept an id from a trusted upstream proxy only if it is well formed;
    // otherwise mint one. v7 ids sort by time, which helps when reading logs.
    const incoming = req.headers["x-request-id"];
    req.id = incoming && isUuid(incoming) ? incoming : v7();
    req.log = root.child({ requestId: req.id });
    req.startedAt = Date.now();
});

// Wrap handlers so every request logs exactly one completion line.
function traced(route, handler) {
    return async (req) => {
        let response, failure = null;
        try { response = await handler(req); }
        catch (e) { failure = e; response = json(500, { error: "internal error", requestId: req.id }); }
        const status = response?.status ?? 200;
        const fields = { route, method: req.method, status, durationMs: Date.now() - req.startedAt };
        if (failure) req.log.error(failure, fields, "request failed");
        else if (status >= 400) req.log.warn(fields, "request rejected");
        else req.log.info(fields, "request completed");
        return response;
    };
}

// Business code logs through the request's logger and never thinks about ids.
function chargeCard(log, order) {
    log.debug({ orderId: order.id }, "contacting payment provider");
    if (order.amount > 1000) throw new Error("payment provider declined: limit exceeded");
    log.info({ orderId: order.id, amount: order.amount }, "payment captured");
}

app.post("/orders", traced("createOrder", (req) => {
    const order = JSON.parse(req.body);
    req.log.info({ headers: redact(req.headers), customer: order.customer }, "order received");
    if (!order.customer) return json(400, { error: "customer is required", requestId: req.id });
    chargeCard(req.log, order);
    return json(201, { id: order.id, requestId: req.id });
}));
app.start();
console.log("orders API on port", app.port, "logging to", logFile ?? "stderr");

// ---- self-test -------------------------------------------------------------
if (selfTest) {
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
    const post = async (body, headers = {}) => {
        const res = await fetch(`http://127.0.0.1:${app.port}/orders`, { method: "POST", body: JSON.stringify(body), headers });
        return { status: res.status, data: await res.json() };
    };
    const upstreamId = "0190a3e2-7b1c-7def-8a3b-1234567890ab";
    const ok = await post({ id: "o-1", customer: "ada", amount: 40 }, { authorization: "Bearer secret-token", "x-request-id": upstreamId });
    const bad = await post({ id: "o-2", amount: 5 });
    const boom = await post({ id: "o-3", customer: "bob", amount: 5000 });
    // A customer name containing a newline and a fake log line.
    await post({ id: "o-4", customer: 'eve"}\n{"level":"error","msg":"FORGED', amount: 1 });

    root.flush();
    // readFile never follows a symlink, so resolve app.log to the active file first.
    const lines = readFile(realPath(dir.join("app.log"))).trim().split("\n");
    const entries = lines.map((l) => JSON.parse(l));                    // every line must be valid JSON
    const of = (id) => entries.filter((e) => e.requestId === id);

    check(ok.status === 201 && bad.status === 400 && boom.status === 500, "three outcomes: 201, 400, 500");
    check(ok.data.requestId === upstreamId, "a well-formed upstream id is kept");
    check(of(upstreamId).length === 3, "all lines of one request share its id (received, captured, completed)");
    check(entries.every((e) => e.service === "orders-api"), "base fields are on every line");
    check(!lines.join("\n").includes("secret-token"), "the bearer token never reaches the log");
    check(of(boom.data.requestId).some((e) => e.msg === "request failed" && JSON.stringify(e).includes("limit exceeded")), "a failure logs the error with its request id");
    check(!boom.data.error.includes("provider"), "the client sees a generic message, the log has the detail");
    check(of(bad.data.requestId).some((e) => e.msg === "request rejected" && e.status === 400), "rejections are logged as warnings");
    check(entries.filter((e) => e.msg === "FORGED").length === 0, "a newline in user data cannot forge a log line");
    check(entries.every((e) => e.msg !== "contacting payment provider"), "debug lines are filtered at level info");
    const completed = entries.filter((e) => typeof e.durationMs === "number");
    check(completed.length === 4, "exactly one completion line per request: " + completed.length);
    console.log(`self-test passed: ${lines.length} structured lines for 4 requests`);
    app.close();
    removeAll(dir);
}
