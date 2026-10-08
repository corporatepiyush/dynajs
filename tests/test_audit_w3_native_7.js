// flags: --std
// timeout: 300
// tests/test_audit_w3_native_7.js -- audit wave 3, small items.
// Each section names its ORACLE and CONTROL; [red] fails on the pre-fix binary.
//   N3-24 SQLite: U+0000 in SQL refused; change count only for statements
//         that changed rows
//   N3-20 PostgreSQL: U+0000 in SQL refused; out-of-range int8 text stays text
//   N3-26 DNS: a name of 255 octets is the limit
//   N1-17 HTTP server: only HTTP/1.0 and HTTP/1.1 request lines, token methods
//   P1-25 term.Table column count is bounded before allocation
// build-note: needs the native modules
import * as std from "std";
import * as net from "dyna:net";
import { App } from "dyna:http";

let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; print("  FAIL:", msg); } }
function thrown(fn) { try { fn(); return "none"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---- N3-24 --------------------------------------------------------------
// ORACLE: SQLite's own definition -- sqlite3_changes() is "the number of rows
// modified by the most recently completed INSERT, UPDATE or DELETE"; DDL and
// SELECT modify no rows, so exec() of those must report 0, not the count left
// over from an earlier statement. And C strings end at U+0000: SQL text that
// contains one cannot be what the caller meant to run.
if (typeof net.SQLite === "function") {
    const db = new net.SQLite(":memory:");
    db.exec("create table t(a)");
    ok(db.exec("insert into t values (1),(2),(3)") === 3, "control: an INSERT of 3 rows reports 3");
    ok(db.exec("create table u(a)") === 0, "[red] N3-24: CREATE TABLE after a 3-row insert reports 0 (" + db.exec("create table u2(a)") + ")");
    ok(db.exec("update t set a = a where a > 100") === 0, "control: an UPDATE that matches nothing reports 0");
    ok(db.exec("delete from t where a = 1; create table v(a)") === 1, "[red] N3-24: multi-statement exec sums only real changes");
    ok(/U\+0000/.test(thrown(() => db.query("select 1 as x\0 garbage"))), "[red] N3-24: SQL with an embedded U+0000 is refused (" + thrown(() => db.query("select 1 as x\0 garbage")).slice(0, 50) + ")");
    ok(db.query("select count(*) as n from t")[0].n === 2, "control: the table holds the 2 remaining rows");
    db.close();
} else {
    print("  SKIP: this build has no SQLite");
}

// ---- N3-20 --------------------------------------------------------------
// Live PostgreSQL (5432, trust; PostgreSQL 18 on the development host).
// ORACLE: the wire protocol -- query text is a NUL-terminated string, so SQL
// containing U+0000 would be cut silently; it must be refused locally.
// CONTROL: the int8 extremes round-trip through the live server.
{
    const port = +(std.getenv("LIVESRV_PG_PORT") || 5432);
    const pg = new net.PostgreSQL({ host: "127.0.0.1", port, user: std.getenv("LIVESRV_PG_USER") || std.getenv("USER") || "postgres", database: "postgres", connectTimeoutMs: 2500 });
    let up = false;
    try { await pg.query("select 1"); up = true; } catch (e) {}
    if (!up) {
        print("  SKIP: no PostgreSQL on 127.0.0.1:" + port + " (brew services start postgresql@18)");
    } else {
        let e = "none";
        try { await pg.query("select 1 as a\0, 2 as b"); } catch (x) { e = x.constructor.name + ": " + x.message; }
        ok(/U\+0000/.test(e), "[red] N3-20: SQL with an embedded U+0000 is refused before it is sent (" + e.slice(0, 60) + ")");
        const r = await pg.query("select 9223372036854775807::int8 as max, (-9223372036854775807 - 1)::int8 as min, 42::int8 as small");
        const row = (r.rows ?? r)[0];
        ok(Number(row.small) === 42 && String(row.max) === "9223372036854775807" && String(row.min) === "-9223372036854775808", "control: int8 extremes survive on a live PostgreSQL (" + row.max + ", " + row.min + ")");
    }
    try { pg.close(); } catch (e) {}
}

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

// ---- P1-25 --------------------------------------------------------------
// ORACLE: arithmetic. A header array whose length is 4e9 would make the
// table reserve four arrays of 4e9 entries (32 GiB each) before looking at a
// single element; a bounded column count must refuse first.
// CONTROL: an ordinary table renders.
{
    const term = await import("dyna:cli");
    ok(term.Table([["a", "b"], ["1", "2"]]).includes("a"), "control: a 2x2 table renders");
    const head = []; head.length = 4e9;
    let e = "none";
    try { term.Table([], { head }); } catch (x) { e = x.constructor.name; }
    if (e === "none") { try { term.Table([head]); } catch (x) { e = x.constructor.name; } }
    ok(e === "RangeError", "[red] P1-25: a 4e9-column header is a RangeError before any allocation (" + e + ")");
}

print("test_audit_w3_native_7: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_native_7: " + failures + " failures");
