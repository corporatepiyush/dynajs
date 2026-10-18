// flags: --std
// timeout: 300
// Split from tests/test_audit_w3_native_7.js: the http sections.
import * as net from "dyna:net";
import { App } from "dyna:http";

let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; print("  FAIL:", msg); } }
function thrown(fn) { try { fn(); return "none"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
// ---- N1-17 --------------------------------------------------------------
// ORACLE: RFC 9112 section 2.3 (HTTP-version = "HTTP/" DIGIT "." DIGIT; a
// server speaks 1.x) and RFC 9110 section 5.6.2 (method = token). A request
// line with another version or a non-token method is not HTTP/1.x: 400.
// CONTROL: the same request with HTTP/1.1 and HTTP/1.0 is served.
{
    const app = new App({ port: 0 });
    app.get("/x", () => "ok");
    app.start();
    function raw(line) {
        return new Promise((resolve) => {
            let buf = "", done = false;
            const fin = () => { if (!done) { done = true; try { cli.close(); } catch (e) {} resolve(buf.split("\r\n")[0]); } };
            const cli = net.TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
                connect: (conn) => { if (!conn) return fin(); conn.write(line + "\r\nHost: a\r\nConnection: close\r\n\r\n"); },
                data: (conn, bytes) => { buf += new TextDecoder().decode(bytes); if (buf.includes("\r\n")) fin(); },
                close: fin,
            });
            setTimeout(fin, 3000);
        });
    }
    ok(/ 200 /.test(await raw("GET /x HTTP/1.1")), "control: HTTP/1.1 is served");
    ok(/ 200 /.test(await raw("GET /x HTTP/1.0")), "control: HTTP/1.0 is served");
    for (const line of ["GET /x HTTP/9.9", "GET /x HTTP/2.0", "GET /x HTTX/1.1", "GET /x XHTTP/1.1", "G(T /x HTTP/1.1", "GE\x01T /x HTTP/1.1"]) {
        const st = await raw(line);
        ok(/ 400 /.test(st), "[red] N1-17: request line " + JSON.stringify(line) + " is a 400 (" + st.slice(0, 40) + ")");
    }
    app.close();
}
print("test_http_audit_w3_native_7: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_http_audit_w3_native_7: " + failures + " failures");
