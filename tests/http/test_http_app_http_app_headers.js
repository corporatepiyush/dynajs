// flags: --std
// covers: dyna:http.App
// The headers object an App handler receives (req.headers). Names are lower-cased, keys keep
// first-seen order, repeated names on the allowed list merge (cookie with "; ", set-cookie keeps
// the first value, the rest with ", "), values are trimmed, and names like __proto__ or
// constructor are plain own data properties (the prototype is untouched). The App builds this
// object from a per-App cache of templates keyed by the exact name sequence once a sequence
// repeats, so every expectation is checked on the first request (no template), the second (the
// template is created) and later ones (template reused, values and order still per request).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_app_http_app_headers");
import * as std from "std";
import { TCPServer } from "dyna:net";
import { App } from "dyna:http";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };

function latin1(u8) { let s = ""; for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]); return s; }
function bytes(s) { const a = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i) & 0xff; return a; }

function ask(port, head) {
  return new Promise((resolve) => {
    let buf = "";
    const t = setTimeout(() => resolve(null), 5000);
    TCPServer.connect({ host: "127.0.0.1", port }, {
      connect: (c, err) => { if (err) { clearTimeout(t); resolve(null); return; } c.write(bytes(head)); },
      data: (c, b) => {
        buf += latin1(b);
        const he = buf.indexOf("\r\n\r\n");
        const m = /Content-Length: (\d+)/i.exec(buf);
        if (he >= 0 && m && buf.length >= he + 4 + Number(m[1])) {
          clearTimeout(t);
          resolve(JSON.parse(buf.substr(he + 4, Number(m[1]))));
          c.close();
        }
      },
      close: () => { clearTimeout(t); resolve(null); },
    });
  });
}

const app = new App({ port: 0, compress: false });
app.get("/h", (r) => {
  const h = r.headers;
  const keys = Object.keys(h);
  return {
    keys,
    vals: keys.map((k) => Object.getOwnPropertyDescriptor(h, k).value),
    proto: Object.getPrototypeOf(h) === Object.prototype,
    own: Object.getOwnPropertyNames(h).length,
  };
});
app.start();
const port = app.port;

const full = (ua, x) =>
  "GET /h HTTP/1.1\r\nHost: a\r\nX-One: " + x + "\r\nAccept: a\r\naccept: b\r\nCookie: c=1\r\ncookie: d=2\r\n" +
  "Set-Cookie: s1\r\nset-cookie: s2\r\n__proto__: p\r\nConstructor: k\r\nUser-Agent:   " + ua + "  \r\n\r\n";
const wantKeys = ["host", "x-one", "accept", "cookie", "set-cookie", "__proto__", "constructor", "user-agent"];

for (let round = 0; round < 4; round++) {
  const ua = "ua" + round, x = String(round * 7);
  const r = await ask(port, full(ua, x));
  ok(r !== null, `round ${round}: a response`);
  if (!r) continue;
  ok(JSON.stringify(r.keys) === JSON.stringify(wantKeys), `round ${round}: keys in first-seen order (${JSON.stringify(r.keys)})`);
  ok(JSON.stringify(r.vals) === JSON.stringify(["a", x, "a, b", "c=1; d=2", "s1", "p", "k", ua]),
    `round ${round}: merged and trimmed values (${JSON.stringify(r.vals)})`);
  ok(r.proto === true, `round ${round}: the prototype is Object.prototype`);
  ok(r.own === wantKeys.length, `round ${round}: exactly ${wantKeys.length} own properties (${r.own})`);
}

for (let round = 0; round < 3; round++) {
  const r = await ask(port, "GET /h HTTP/1.1\r\nUser-Agent: z" + round + "\r\nHost: b\r\nX-One: q\r\n\r\n");
  ok(r && JSON.stringify(r.keys) === JSON.stringify(["user-agent", "host", "x-one"]) &&
     JSON.stringify(r.vals) === JSON.stringify(["z" + round, "b", "q"]),
     `reordered names, round ${round}: own order and values (${r && JSON.stringify(r)})`);
}

for (let round = 0; round < 3; round++) {
  const r = await ask(port, "GET /h HTTP/1.1\r\nHost: c\r\nX-Two: " + round + "\r\n123: num\r\n\r\n");
  ok(r && JSON.stringify(r.keys) === JSON.stringify(["123", "host", "x-two"]) &&
     JSON.stringify(r.vals) === JSON.stringify(["num", "c", String(round)]),
     `integer-like name, round ${round}: enumerates first, values intact (${r && JSON.stringify(r)})`);
}

await __kitS.section("GET /h HTTP/1.1r Host: dr", async () => {
  const names = [];
  for (let k = 0; k < 6; k++) names.push("N" + k);
  for (let round = 0; round < 12; round++) {
    const sel = names.slice(0, 1 + (round % 6));
    let head = "GET /h HTTP/1.1\r\nHost: d\r\n";
    for (const nm of sel) head += nm + ": " + nm + round + "\r\n";
    const r = await ask(port, head + "\r\n");
    const want = ["host", ...sel.map((s) => s.toLowerCase())];
    ok(r && JSON.stringify(r.keys) === JSON.stringify(want) && r.vals[r.vals.length - 1] === sel[sel.length - 1] + round,
      `six alternating name sets (more than the template cache holds), round ${round} (${r && JSON.stringify(r.keys)})`);
  }
});
__kitS.end();

app.close();
print(`test_http_app_headers: ${pass} passed, ${fail} failed`);
if (fail) std.exit(1);
