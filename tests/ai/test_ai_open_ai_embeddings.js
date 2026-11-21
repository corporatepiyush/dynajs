// timeout: 120
// covers: dyna:ai.OpenAI
// OpenAI embeddings.create against an in-process dyna:http App speaking
// POST {baseURL}/embeddings. When the caller sets no encoding_format the
// client asks for "base64" (4 bytes per float instead of ~20 JSON characters)
// and hands back each data[i].embedding as a Float32Array decoded natively
// (little-endian float32); a server that ignores the request and sends number
// arrays still yields Float32Array. An explicit encoding_format is sent and
// returned as given. Bad base64, a byte length that is not a multiple of 4, a
// non-number element or a missing data array reject with APIError api_error.
import { sections as __kitSections } from "../lib/kit.js";
import { OpenAI, APIError } from "dyna:ai";
import { App } from "dyna:http";
const __kitS = __kitSections("test_ai_open_ai_embeddings", { chain: true });
let plan, app, c;

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d !== undefined ? "  [" + d + "]" : "")); } };
const rejects = async (p, pred, w) => {
    try { await p; ok(false, w, "resolved"); }
    catch (e) { ok(pred(e), w, e && (e.constructor.name + ": " + e.message + (e.status !== undefined ? " status=" + e.status + " type=" + e.type : ""))); }
};
const VECS = [[0.5, -1.25, 0.1, 3e38], [1e-45, -0, 1, 2]];
const b64 = (v) => new Uint8Array(new Float32Array(v).buffer).toBase64();
const reply = (data) => ({ status: 200, contentType: "application/json", body: JSON.stringify({ object: "list", model: "emb", data, usage: { prompt_tokens: 4, total_tokens: 4 } }) });
const items = (enc) => VECS.map((v, i) => ({ object: "embedding", index: i, embedding: enc(v) }));

const log = [];
const last = () => log[log.length - 1];
const same = (f32, v) => f32 instanceof Float32Array && f32.length === v.length && v.every((x, i) => Object.is(f32[i], Math.fround(x)));
__kitS.section("fixture: create the scripted response plan", () => {
plan = new Map();
app = new App({ port: 0, host: "127.0.0.1" });
app.post("/v1/embeddings", (req) => {
    const b = JSON.parse(req.body);
    log.push({ path: req.path, headers: req.headers, body: b });
    const step = plan.get(b.model);
    if (step) return step(b);
    if (b.encoding_format === "base64") return reply(items(b64));
    return reply(items((v) => v));
});
app.start();
c = new OpenAI({ apiKey: "test-key", baseURL: "http://127.0.0.1:" + app.port + "/v1", maxRetries: 0 });
});

try {
    {
let r;
await __kitS.section("posts to {baseURL}/embeddings with Bearer auth", async () => {
        r = await c.embeddings.create({ model: "emb", input: ["a", "b"], dimensions: 4, user: "u1" });
        ok(last().path === "/v1/embeddings" && last().headers["authorization"] === "Bearer test-key", "posts to {baseURL}/embeddings with Bearer auth");
        ok(last().body.encoding_format === "base64" && last().body.dimensions === 4 && last().body.user === "u1" && JSON.stringify(last().body.input) === '["a","b"]',
            "no encoding_format: the client asks for base64; other keys pass through", JSON.stringify(last().body));
        ok(r.data.length === 2 && same(r.data[0].embedding, VECS[0]) && same(r.data[1].embedding, VECS[1]), "base64 embeddings decode to Float32Array, bit for bit (0.1 rounds, -0 and denormals kept)",
            r.data.map((d) => Array.from(d.embedding).join(",")).join(" | "));
        ok(r.data[1].index === 1 && r.data[0].object === "embedding" && r.usage.total_tokens === 4 && r.model === "emb", "index, object, model and usage pass through");
});
    }
    {
let r, J, e;
await __kitS.section("a server that answers number arrays still yields Float32Array", async () => {
        plan.set("emb-arr", () => reply(items((v) => v)));
        r = await c.embeddings.create({ model: "emb-arr", input: "x" });
        J = JSON.parse(JSON.stringify(VECS));
        ok(same(r.data[0].embedding, J[0]) && same(r.data[1].embedding, J[1]), "a server that answers number arrays still yields Float32Array (values as JSON carried them)");
});
await __kitS.section("an empty embedding is an empty Float32Array", async () => {
        plan.set("emb-empty", () => reply([{ object: "embedding", index: 0, embedding: "" }]));
        e = await c.embeddings.create({ model: "emb-empty", input: "x" });
        ok(e.data[0].embedding instanceof Float32Array && e.data[0].embedding.length === 0, "an empty embedding is an empty Float32Array");
});
    }
    {
let r, r2;
await __kitS.section("encoding_format float is sent and returned as given (number[])", async () => {
        r = await c.embeddings.create({ model: "emb", input: "x", encoding_format: "float" });
        ok(last().body.encoding_format === "float" && Array.isArray(r.data[0].embedding) && r.data[0].embedding[2] === 0.1, "encoding_format float is sent and returned as given (number[])");
});
await __kitS.section("an explicit encoding_format base64 returns the base64 text untouched", async () => {
        r2 = await c.embeddings.create({ model: "emb", input: "x", encoding_format: "base64" });
        ok(typeof r2.data[0].embedding === "string" && r2.data[0].embedding === b64(VECS[0]), "an explicit encoding_format base64 returns the base64 text untouched");
});
    }
    {
        const bad = (name, emb, re, w) => { plan.set(name, () => reply([{ object: "embedding", index: 0, embedding: emb }])); return rejects(c.embeddings.create({ model: name, input: "x" }),
            (e) => e instanceof APIError && e.type === "api_error" && e.status === 200 && re.test(e.message), w); };
await __kitS.section("a response without data rejects", async () => {
        await bad("emb-b64", "!!!!", /data\[0\]: not base64/, "bad base64 rejects with APIError api_error");
        await bad("emb-len", "AAA=", /multiple of 4/, "a byte length that is not a multiple of 4 rejects");
        await bad("emb-nan", [1, "2"], /not a number/, "a non-number element rejects");
        await bad("emb-obj", { x: 1 }, /neither base64 text nor an array/, "an embedding of another type rejects");
        plan.set("emb-nodata", () => ({ status: 200, contentType: "application/json", body: "{\"object\":\"list\"}" }));
        await rejects(c.embeddings.create({ model: "emb-nodata", input: "x" }), (e) => e instanceof APIError && /no data array/.test(e.message), "a response without data rejects");
});
await __kitS.section("an HTTP 400 is an APIError", async () => {
        plan.set("emb-400", () => ({ status: 400, contentType: "application/json", body: JSON.stringify({ error: { message: "input too long", type: "invalid_request_error", param: "input", code: null } }) }));
        await rejects(c.embeddings.create({ model: "emb-400", input: "x" }), (e) => e instanceof APIError && e.status === 400 && e.body.error.param === "input", "an HTTP 400 is an APIError");
});
    }
    {
await __kitS.section("model is required", async () => {
        await rejects(c.embeddings.create({ input: "x" }), (e) => e instanceof TypeError && /no default model/.test(e.message), "model is required");
        await rejects(c.embeddings.create({ model: "emb" }), (e) => e instanceof TypeError && /input must be a string or an array/.test(e.message), "input is required");
        await rejects(c.embeddings.create({ model: "emb", input: 5 }), (e) => e instanceof TypeError, "a numeric input is refused");
        await rejects(c.embeddings.create("emb"), (e) => e instanceof TypeError && /body must be an object/.test(e.message), "a non-object body is refused");
});
    }
} finally { if (!__kitS.listing) {
    c.close();
    app.close();
} }
__kitS.end();
if (!__kitS.listing) {
print(`test_ai_open_ai_embeddings: ${pass} passed, ${fail} failed`);
if (fail) throw new Error("test_ai_open_ai_embeddings failed");
}
