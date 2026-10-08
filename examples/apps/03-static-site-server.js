// 03 · Static site server — serves a document root with an allow-list, size caps and a JSON status route.
//
// WHAT IT SHOWS
//   - App.static: path traversal, dotfiles and symlinks are refused by the server itself
//   - an allow-list of extensions, so an accidental .env or .sql in the root is never served
//   - a typed static prefix living beside ordinary dynamic routes
//
// RUN      dynajs examples/apps/03-static-site-server.js
// DEPLOY   PORT=8080 ROOT=/srv/www dynajs examples/apps/03-static-site-server.js

import { App } from "dyna:net";
import { Path, makeTempDir, makeDir, writeFile, readFile, removeAll, readDir } from "dyna:file";
import { getEnv } from "dyna:sys";

const PORT = Number(getEnv("PORT") ?? 0);
const selfTest = !PORT;

// In the self-test we build a small site in a temp directory; in production
// ROOT points at the real document root.
const root = selfTest ? makeTempDir("site") : new Path(getEnv("ROOT") ?? "./public");
if (selfTest) {
    writeFile(root.join("index.html"), "<!doctype html><h1>It works</h1>");
    writeFile(root.join("app.css"), "body { font-family: sans-serif }");
    writeFile(root.join("notes.sql"), "-- must never be served");
    makeDir(root.join("assets"));
    writeFile(root.join("assets", "data.json"), '{"ok":true}');
    writeFile(root.join(".env"), "SECRET=1");
}

const startedAt = Date.now();
const app = new App({ port: PORT });

// The home page is a dynamic route that returns the index document.
app.get("/", () => ({ contentType: "text/html", body: readFile(root.join("index.html")) }));

// Dynamic routes coexist with the static prefix; this one is for load balancers.
app.get("/_status", () => ({
    uptimeSec: Math.round((Date.now() - startedAt) / 1000),
    files: readDir(root).filter((e) => e.isFile).length,
}));

// Only these types leave the building. Everything else in the root is a 403,
// which turns "someone copied a backup into the web root" into a non-event.
// A static prefix owns EVERYTHING beneath it, so it gets its own prefix and
// the dynamic routes keep "/" for themselves.
app.static("/static", root, {
    allow: [".html", ".css", ".js", ".json", ".png", ".svg", ".ico", ".txt"],
    maxFileSize: 8 * 1024 * 1024,
});

app.start();
console.log(`serving ${root} on port ${app.port}`);

// ---- self-test -------------------------------------------------------------
if (selfTest) {
    const base = `http://127.0.0.1:${app.port}`;
    const get = async (path) => {
        const res = await fetch(base + path);
        return { status: res.status, type: res.headers.get("content-type") ?? "", text: await res.text() };
    };
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };

    const page = await get("/static/index.html");
    check(page.status === 200 && page.text.includes("It works"), "html is served");
    check(page.type.startsWith("text/html"), "html has the right content type");
    check((await get("/")).text.includes("It works"), "the home route returns the index");
    check((await get("/static/assets/data.json")).text === '{"ok":true}', "nested files are served");
    check((await get("/static/notes.sql")).status === 403, "a type outside the allow-list is 403");
    check((await get("/static/.env")).status === 404, "dotfiles are invisible");
    check((await get("/static/../etc/passwd")).status !== 200, "traversal never reaches outside the root");
    check((await get("/static/missing.html")).status === 404, "missing files are 404");
    const status = JSON.parse((await get("/_status")).text);
    check(status.files >= 3, "the dynamic status route works beside the static one");
    console.log("self-test passed: allow-list, dotfiles, traversal and status all behave");
    app.close();
    removeAll(root);
}
