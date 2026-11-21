// flags: --std
// timeout: 60
// covers: dyna:smtp.SmtpClient -- an idle SmtpClient (no connection, nothing queued) never keeps the event loop alive; it holds the loop only while it has work
// The client acquired the reactor strongly in its constructor, so a script that kept an unclosed client after a failed
// send() (connections 0, queued 0) never exited. It now holds the reactor weakly and strongly only while self-held
// (queued or connected). Oracle: each child ends by itself (rc 0, no timeout) and prints what it must; the blackhole
// child must stay alive until its send settles; the reuse child sends again after the pooled connection idled out.
import { SmtpClient, SmtpServer } from "dyna:smtp";
import * as std from "std";
import * as os from "os";
import { Exec, args } from "dyna:sys";

const SELF = "tests/smtp/test_smtp_smtp_client_idle_exit.js";
const H = "127.0.0.1";
const M = { from: "a@b.test", to: "x@y.test", data: "Subject: t\r\n\r\nhi\r\n" };

function child(mode) {
    if (mode === "kept") { globalThis.c = new SmtpClient({ host: H, port: 1, tls: "none" }); print("CHILD made"); return; }
    if (mode === "failed") {
        globalThis.c = new SmtpClient({ host: H, port: 1, tls: "none", connectTimeoutMs: 300 });
        c.send(M).then(() => print("CHILD unexpected"), (e) => print("CHILD rejected " + e.code));
        return;
    }
    if (mode === "blackhole") {
        globalThis.c = new SmtpClient({ host: "10.255.255.1", port: 25, tls: "none", connectTimeoutMs: 400 });
        const t0 = Date.now();
        c.send(M).then(() => print("CHILD unexpected"), (e) => print("CHILD rejected " + e.code + " after " + (Date.now() - t0)));
        return;
    }
    if (mode === "reuse") {
        const s = new SmtpServer({ port: 0, tls: "none", name: "mx.test", host: H });
        s.listen();
        const port = s.port;
        globalThis.c = new SmtpClient({ host: H, port, tls: "none", name: "c.test", idleTimeoutMs: 100 });
        c.send(M).then((r) => {
            print("CHILD first " + r.accepted.length);
            os.setTimeout(() => {
                print("CHILD idle connections " + c.stats().connections);
                s.close();
                const s2 = new SmtpServer({ port, tls: "none", name: "mx.test", host: H });
                s2.listen();
                c.send(M).then((r2) => { print("CHILD second " + r2.accepted.length); s2.close(); }, (e) => { print("CHILD second failed " + e); s2.close(); });
            }, 400);
        }, (e) => { print("CHILD first failed " + e); s.close(); });
    }
}

function main() {
    let n = 0, fails = 0;
    const check = (ok, m) => { n++; if (!ok) { fails++; print("FAIL: " + m); } };
    const want = { kept: /CHILD made/, failed: /CHILD rejected SMTP_CONNECTION/, blackhole: /CHILD rejected SMTP_(TIMEOUT|CONNECTION)/, reuse: /CHILD first 1[\s\S]*CHILD idle connections 0[\s\S]*CHILD second 1/ };
    for (const mode of Object.keys(want)) {
        const t0 = Date.now();
        const r = Exec(args()[0], ["--std", SELF, "--child", mode], { timeoutMs: 10000 });
        const ms = Date.now() - t0;
        print(mode + ": rc " + r.code + " in " + ms + " ms :: " + r.stdout.trim().replace(/\n/g, " | "));
        check(!r.timedOut && r.code === 0, mode + " exits by itself (rc " + r.code + ", timedOut " + r.timedOut + ") " + r.stderr.slice(-200));
        check(want[mode].test(r.stdout), mode + " output: " + r.stdout);
    }
    print("smtp_client_idle_exit: " + n + " checks, " + fails + " failed");
    if (fails) std.exit(1);
}

if (args().includes("--child")) child(args()[args().indexOf("--child") + 1]);
else main();
