// covers: dyna:smtp.SmtpClient
// test_smtp_smtp_client_options.js -- SmtpClient constructor and send() argument contract (no network):
// unknown keys, enum values, integer ranges, CR/LF/NUL in any string option or address (header and
// command injection), recipient count 1..1000, data types. S9 S1b, 2026-11-27.
import { suite } from "../lib/kit.js";
import { SmtpClient } from "dyna:smtp";

const t = suite("test_smtp_smtp_client_options");
const H = { host: "127.0.0.1", port: 1, tls: "none" };

t.test("constructor validation", (a) => {
    a.throws(() => new SmtpClient(), TypeError, "no options");
    a.throws(() => new SmtpClient({}), /host is required/);
    a.throws(() => new SmtpClient({ ...H, bogus: 1 }), TypeError, "unknown key");
    a.throws(() => new SmtpClient({ ...H, tls: "yes" }), RangeError);
    a.throws(() => new SmtpClient({ ...H, port: 0 }), RangeError);
    a.throws(() => new SmtpClient({ ...H, port: 70000 }), RangeError);
    a.throws(() => new SmtpClient({ ...H, maxConnections: 65 }), RangeError);
    a.throws(() => new SmtpClient({ ...H, scramMaxIterations: 100 }), RangeError, "below the 4096 floor");
    a.throws(() => new SmtpClient({ ...H, name: "bad name" }), /name must be/);
    a.throws(() => new SmtpClient({ ...H, name: "x.test\r\nRSET" }), /CR or LF/);
    a.throws(() => new SmtpClient({ ...H, user: "u\r\nMAIL FROM:<x>", pass: "p" }), /CR or LF/);
    a.throws(() => new SmtpClient({ ...H, user: "u" }), /user with pass or token/);
    a.throws(() => new SmtpClient({ ...H, pass: "p" }), /user with pass or token/);
    a.throws(() => new SmtpClient({ ...H, mechanisms: ["GSSAPI"] }), RangeError);
    a.throws(() => new SmtpClient({ ...H, mechanisms: [] }), RangeError);
    a.throws(() => new SmtpClient({ ...H, host: 5 }), TypeError);
    const c = new SmtpClient({ ...H, name: "[192.0.2.1]", mechanisms: ["SCRAM-SHA-256", "PLAIN"], maxConnections: 64 });
    a.ok(!c.closed);
    c.close();
    a.ok(c.closed);
    c.close();
});

t.test("send() validation throws synchronously", (a) => {
    const c = new SmtpClient(H);
    const ok = { from: "a@b.test", to: "x@y.test", data: "x" };
    a.throws(() => c.send(), TypeError);
    a.throws(() => c.send({ ...ok, extra: 1 }), TypeError);
    a.throws(() => c.send({ ...ok, from: 1 }), TypeError);
    a.throws(() => c.send({ ...ok, from: "a@b.test>\r\nRCPT TO:<evil@x" }), /CR, LF/);
    a.throws(() => c.send({ ...ok, to: ["x@y.test", "y@z\n"] }), /CR, LF/);
    a.throws(() => c.send({ ...ok, to: [] }), RangeError);
    a.throws(() => c.send({ ...ok, to: new Array(1001).fill("x@y.test") }), RangeError);
    a.throws(() => c.send({ ...ok, to: 5 }), TypeError);
    a.throws(() => c.send({ ...ok, data: 5 }), TypeError);
    a.throws(() => c.send({ ...ok, dsn: { bogus: 1 } }), TypeError);
    a.throws(() => c.send({ ...ok, dsn: { notify: "SUCCESS" } }), TypeError);
    a.throws(() => c.send({ ...ok, from: "a".repeat(300) + "@b.test" }), /256/);
    c.close();
    a.throws(() => c.send(ok), Error, "after close");
});

await t.run();
