// 01 · REST API over SQLite — a CRUD service with validation, pagination and JSON errors.
//
// WHAT IT SHOWS
//   - dyna:net App: dynamic routes with :params, query strings and JSON envelopes
//   - dyna:net SQLite: bound parameters only (never string-built SQL)
//   - dyna:schema: one compiled JSON Schema guards every write
//
// RUN      dynajs examples/apps/01-rest-api-sqlite.js
// DEPLOY   PORT=8080 DB=/var/lib/notes.db dynajs examples/apps/01-rest-api-sqlite.js
//          With PORT set the service runs until stopped; without it the file
//          starts on a free port, exercises itself and exits (the self-test).

import { App, SQLite } from "dyna:net";
import { Schema } from "dyna:schema";
import { getEnv } from "dyna:sys";

const PORT = Number(getEnv("PORT") ?? 0);
const db = new SQLite(getEnv("DB") ?? ":memory:");

// One table, created idempotently so a restart against an existing file is safe.
db.exec(`CREATE TABLE IF NOT EXISTS notes(
           id INTEGER PRIMARY KEY AUTOINCREMENT,
           title TEXT NOT NULL,
           body  TEXT NOT NULL DEFAULT '',
           done  INTEGER NOT NULL DEFAULT 0,
           created_at INTEGER NOT NULL)`);

// The schema is compiled once; validate() afterwards is pure dispatch.
const noteSchema = Schema.compile({
    type: "object",
    required: ["title"],
    additionalProperties: false,
    properties: {
        title: { type: "string", minLength: 1, maxLength: 200 },
        body:  { type: "string", maxLength: 10000 },
        done:  { type: "boolean" },
    },
});

// Small helpers so every handler returns the same envelope shape.
const json = (status, value) =>
    ({ status, contentType: "application/json", body: JSON.stringify(value) });
const fail = (status, message, details) => json(status, { error: message, details });

function parseBody(req) {
    let value;
    try { value = JSON.parse(req.body); }
    catch (e) { return { error: fail(400, "body is not valid JSON") }; }
    const verdict = noteSchema.validate(value);
    if (!verdict.valid) return { error: fail(422, "validation failed", verdict.errors) };
    return { value };
}

const rowToNote = (r) => ({ id: r.id, title: r.title, body: r.body, done: !!r.done, createdAt: r.created_at });

const app = new App({ port: PORT });

// LIST with pagination. limit is clamped: a client can never ask for the table.
app.get("/notes", (req) => {
    const limit = Math.min(Math.max(Number(req.query.limit ?? 20) | 0, 1), 100);
    const offset = Math.max(Number(req.query.offset ?? 0) | 0, 0);
    const rows = db.query("SELECT * FROM notes ORDER BY id LIMIT ? OFFSET ?", [limit, offset]);
    const total = db.query("SELECT COUNT(*) AS n FROM notes")[0].n;
    return json(200, { total, limit, offset, items: rows.map(rowToNote) });
});

app.get("/notes/:id", (req) => {
    const rows = db.query("SELECT * FROM notes WHERE id = ?", [Number(req.params.id)]);
    return rows.length ? json(200, rowToNote(rows[0])) : fail(404, "no such note");
});

app.post("/notes", (req) => {
    const { value, error } = parseBody(req);
    if (error) return error;
    db.exec("INSERT INTO notes(title, body, done, created_at) VALUES (?, ?, ?, ?)",
            [value.title, value.body ?? "", value.done ? 1 : 0, Date.now()]);
    return json(201, { id: db.lastInsertRowId });
});

app.put("/notes/:id", (req) => {
    const { value, error } = parseBody(req);
    if (error) return error;
    // exec() returns the rows changed, which is exactly the "did it exist" answer.
    const changed = db.exec("UPDATE notes SET title = ?, body = ?, done = ? WHERE id = ?",
                            [value.title, value.body ?? "", value.done ? 1 : 0, Number(req.params.id)]);
    return changed ? json(200, { updated: true }) : fail(404, "no such note");
});

app.del("/notes/:id", (req) => {
    const changed = db.exec("DELETE FROM notes WHERE id = ?", [Number(req.params.id)]);
    return changed ? json(200, { deleted: true }) : fail(404, "no such note");
});

app.start();
console.log("notes API listening on port", app.port);

// ---- self-test: runs only when PORT is not set -----------------------------
if (!PORT) {
    const base = `http://127.0.0.1:${app.port}`;
    const call = async (method, path, body) => {
        const res = await fetch(base + path, { method, body: body && JSON.stringify(body) });
        return { status: res.status, data: await res.json() };
    };
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };

    const created = await call("POST", "/notes", { title: "buy milk" });
    check(created.status === 201 && created.data.id === 1, "create");
    await call("POST", "/notes", { title: "write report", body: "Q3 numbers" });

    const bad = await call("POST", "/notes", { title: "" });
    check(bad.status === 422, "empty title is refused with 422");
    const typo = await call("POST", "/notes", { title: "x", bodyy: "typo" });
    check(typo.status === 422, "unknown property is refused");

    check((await call("PUT", "/notes/1", { title: "buy oat milk", done: true })).status === 200, "update");
    const one = await call("GET", "/notes/1");
    check(one.data.title === "buy oat milk" && one.data.done === true, "read back");

    const page = await call("GET", "/notes?limit=1&offset=1");
    check(page.data.total === 2 && page.data.items.length === 1, "pagination");

    check((await call("DELETE", "/notes/2")).status === 200, "delete");
    check((await call("GET", "/notes/2")).status === 404, "deleted note is gone");
    console.log("self-test passed: create, validate, update, paginate, delete");
    app.close();
    db.close();
}
