// flags: --std
// timeout: 300
// tests/test_audit_w3_native_5.js -- audit wave 3, batch 5.
// Each section names its ORACLE and CONTROL; [red] fails on the pre-fix binary.
//   N3-17 PostgreSQL `tls: true` negotiates with SSLRequest
//   M1c-19 dyna:simd window arguments are strict integers
// build-note: needs the native modules and CONFIG_TLS=y
import * as std from "std";
import { PostgreSQL, TCPServer } from "dyna:net";
import { Path, exists } from "dyna:file";

let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; print("  FAIL:", msg); } }

// ---- N3-17 --------------------------------------------------------------
// ORACLE: the PostgreSQL frontend/backend protocol ("SSL Session
// Encryption"): the client first sends the 8-byte SSLRequest (length 8, code
// 80877103); the server answers ONE byte, 'S' or 'N'; TLS starts only after
// 'S', and any further byte in that answer is an injection attempt
// (CVE-2021-23222 class) that must end the connection.
// The mock records exactly what the client sent first.
async function mock(answer) {
    let first = null;
    const srv = new TCPServer({ port: 0 });
    srv.start({ data: (conn, bytes) => { if (!first) { first = Array.from(bytes); conn.write(answer); } } });
    const c = new PostgreSQL({ host: "127.0.0.1", port: srv.port, user: "u", password: "p", database: "d", tls: true, connectTimeoutMs: 3000 });
    let err = "none";
    try { await c.query("select 1"); } catch (e) { err = String(e.message); }
    try { c.close(); } catch (e) {}
    srv.close();
    return { first, err };
}
{
    const n = await mock("N");
    ok(n.first && n.first.join() === "0,0,0,8,4,210,22,47", "[red] N3-17: the first bytes on the wire are the SSLRequest (" + (n.first || []).slice(0, 8).join() + ")");
    ok(/does not accept TLS/.test(n.err), "[red] N3-17: an 'N' answer fails closed, naming the reason (" + n.err.slice(0, 70) + ")");
    const extra = await mock("S\x00\x00\x00\x08");
    ok(/unexpected bytes/.test(extra.err), "[red] N3-17: bytes after 'S' are refused before any TLS (" + extra.err.slice(0, 70) + ")");
    const junk = await mock("E");
    ok(/unexpected bytes/.test(junk.err), "[red] N3-17: any other answer byte is refused (" + junk.err.slice(0, 60) + ")");
}

// Live PostgreSQL 18 with ssl = on (see .agent-work/PG-REDIS-INFO.md).
// ORACLE: the server's own view -- pg_stat_ssl reports ssl = true and the
// TLS version for this backend. CONTROL: the same server without `tls`
// reports ssl = false. Skipped loudly when the instance or its CA is absent.
{
    const port = +(std.getenv("LIVESRV_PG_SCRAM_PORT") || 55432);
    const pw = std.getenv("LIVESRV_PG_PASSWORD") || "S3cret!pass";
    const ca = std.getenv("LIVESRV_PG_CA") || ".agent-work/pg18scram/ca.crt";
    const base = { host: "127.0.0.1", port, user: std.getenv("LIVESRV_PG_USER") || "piyush", password: pw, database: "postgres", connectTimeoutMs: 3000 };
    let up = false;
    {
        const p = new PostgreSQL(base);
        try { await p.query("select 1"); up = true; } catch (e) {}
        try { p.close(); } catch (e) {}
    }
    if (!up || !exists(new Path(ca))) {
        print("  SKIP: no TLS-enabled PostgreSQL on 127.0.0.1:" + port + " with CA " + ca + "; live N3-17 rows not run");
    } else {
        const q = "select s.ssl, s.version, current_setting('server_version_num')::int as v from pg_stat_ssl s where s.pid = pg_backend_pid()";
        const plain = new PostgreSQL(base);
        const pr = (await plain.query(q)).rows[0];
        plain.close();
        ok(pr.ssl === false, "control: without tls the server reports ssl = false");
        const sec = new PostgreSQL({ ...base, tls: true, ca });
        let sr = null, err = "none";
        try { sr = (await sec.query(q)).rows[0]; } catch (e) { err = String(e.message); }
        try { sec.close(); } catch (e) {}
        ok(sr && sr.ssl === true && /^TLSv1\.[23]$/.test(sr.version), "[red] N3-17 live: tls: true reaches PostgreSQL " + (sr ? sr.v : "?") + " over " + (sr ? sr.version : "nothing") + " (" + err.slice(0, 60) + ")");
        const wrong = new PostgreSQL({ ...base, host: "localhost", tls: true, ca: "tests/test_audit_w3_native_5.js" });
        let werr = "none";
        try { await wrong.query("select 1"); } catch (e) { werr = String(e.message); }
        try { wrong.close(); } catch (e) {}
        ok(werr !== "none", "control: a CA file that does not sign the server certificate is refused (" + werr.slice(0, 60) + ")");
    }
}

// ---- M1c-19 -------------------------------------------------------------
// ORACLE: the window is "elements offset .. offset+length". A value beyond
// 2^53 names no element and must be refused, not wrapped modulo 2^64 (which
// turned 2^64+1 into offset 0... and returned a sum). CONTROLS: an integer
// window sums exactly; a fractional argument truncates toward zero, the
// contract tests/test_perf_adversarial.js pins.
{
    const simd = await import("dyna:simd");
    const a = Float32Array.of(1, 2, 3, 4, 5);
    ok(simd.sum(a, 1, 3) === 9, "control: sum over the window [1, 4) is 9");
    ok(simd.sum(a, 1.9, 2.9) === 5, "control: a fractional window truncates (elements 1 and 2)");
    for (const [off, len] of [[2 ** 64 + 1, 1], [0, 2 ** 64 + 2], [-1, 1]]) {
        let e = "none";
        try { simd.sum(a, off, len); } catch (x) { e = x.constructor.name; }
        ok(e === "RangeError", (off === -1 ? "control: " : "[red] ") + "M1c-19: sum(a, " + off + ", " + len + ") is a RangeError (" + e + ")");
    }
}

print("test_audit_w3_native_5: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_native_5: " + failures + " failures");
