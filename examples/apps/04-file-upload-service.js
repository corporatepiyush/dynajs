// 04 · File upload service — accepts uploads with type and size limits, stores them by content hash.
//
// WHAT IT SHOWS
//   - App.upload: the body is streamed to disk by the server, never buffered in JS
//   - refusing by declared media type (415) and by size (413) before your code runs
//   - content addressing: the stored name is the SHA-256 of the bytes, so duplicates collapse
//
// RUN      dynajs examples/apps/04-file-upload-service.js
// DEPLOY   PORT=8080 UPLOAD_DIR=/var/lib/uploads dynajs examples/apps/04-file-upload-service.js

import { App } from "dyna:net";
import { Path, makeTempDir, makeDir, readBytes, move, exists, remove, removeAll, readDir } from "dyna:file";
import { SHA256Hex } from "dyna:hash";
import { getEnv } from "dyna:sys";

const PORT = Number(getEnv("PORT") ?? 0);
const selfTest = !PORT;
const dir = selfTest ? makeTempDir("uploads") : new Path(getEnv("UPLOAD_DIR") ?? "./uploads");
const incoming = dir.join("incoming");           // where the server writes first
const store = dir.join("store");                 // content-addressed final home
makeDir(incoming, { recursive: true });
makeDir(store, { recursive: true });

const MAX_BYTES = 2 * 1024 * 1024;
const index = new Map();                         // digest -> { size, contentType, uploads }

const app = new App({ port: PORT });

// The handler runs once the whole body is on disk. A body over maxFileSize
// or with a media type outside `allow` never reaches it.
app.upload("/upload", { dir: incoming, maxFileSize: MAX_BYTES, allow: ["image/png", "text/plain", "application/pdf"] },
    (savedPath, meta) => {
        const tmp = new Path(savedPath);
        const digest = SHA256Hex(readBytes(tmp));
        const dest = store.join(digest);
        if (exists(dest)) remove(tmp);           // same bytes already stored
        else move(tmp, dest);
        const entry = index.get(digest) ?? { size: meta.size, contentType: meta.contentType, uploads: 0 };
        entry.uploads++;
        index.set(digest, entry);
    });

// A listing route so clients can discover what is stored.
app.get("/files", () => [...index].map(([digest, e]) => ({ digest, ...e })));
app.start();
console.log("upload service on port", app.port, "storing under", String(store));

// ---- self-test -------------------------------------------------------------
if (selfTest) {
    const url = `http://127.0.0.1:${app.port}/upload`;
    const put = (body, type) => fetch(url, { method: "POST", body, headers: { "Content-Type": type } });
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };

    const text = "quarterly report\n".repeat(100);
    check((await put(text, "text/plain")).ok, "a text upload is accepted");
    check((await put(text, "text/plain")).ok, "the same bytes upload again");
    check((await put("#!/bin/sh\nrm -rf /", "application/x-sh")).status === 415, "a disallowed type is 415");
    // The server answers 413 from the declared length and may hang up before the
    // client has finished sending, so a refused send counts as the same refusal.
    const tooBig = await put(new Uint8Array(MAX_BYTES + 1), "text/plain").then((r) => r.status, () => 413);
    check(tooBig === 413, "an oversized body is refused");

    const files = await (await fetch(`http://127.0.0.1:${app.port}/files`)).json();
    check(files.length === 1 && files[0].uploads === 2, "duplicates collapse to one stored object");
    check(files[0].digest === SHA256Hex(text), "the stored name is the content digest");
    check(readDir(store).length === 1 && readDir(incoming).length === 0, "nothing is left in incoming");
    console.log("self-test passed: stored", files[0].size, "bytes as", files[0].digest.slice(0, 16) + "...");
    app.close();
    removeAll(dir);
}
