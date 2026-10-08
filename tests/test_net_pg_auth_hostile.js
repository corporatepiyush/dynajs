// flags: --std
// timeout: 120
import { PostgreSQL } from "dyna:net";
import { Exec, Which } from "dyna:sys";
import { makeTempDir, writeFile, readFile, Path } from "dyna:file";

let pass = 0, fail = 0, skip = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; console.log("  FAIL " + w + (d ? "  [" + d + "]" : "")); } };

async function main() {
    if (!Which("python3")) {
        skip++;
        console.log("test_net_pg_auth_hostile: python3 missing, fake server skipped");
        return;
    }
    const T = makeTempDir("d2pgauth");
    const P = (n) => T + "/" + n;
    const sh = (c) => Exec("/bin/sh", ["-c", c]).code;

    writeFile(new Path(P("fakepg.py")), [
        "import socket, struct, sys, threading",
        "mode = sys.argv[3]",
        "srv = socket.socket()",
        "srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)",
        "srv.bind(('127.0.0.1', 0))",
        "srv.listen(8)",
        "open(sys.argv[1], 'w').write(str(srv.getsockname()[1]))",
        "log = open(sys.argv[2], 'ab', buffering=0)",
        "def send(c, typ, payload):",
        "    c.sendall(typ + struct.pack('!I', len(payload) + 4) + payload)",
        "def recv_exact(c, n):",
        "    b = b''",
        "    while len(b) < n:",
        "        ch = c.recv(n - len(b))",
        "        if not ch: return None",
        "        b += ch",
        "    return b",
        "def handle(c):",
        "    c.settimeout(5)",
        "    try:",
        "        hdr = recv_exact(c, 4)",
        "        if not hdr: return",
        "        ln = struct.unpack('!I', hdr)[0]",
        "        body = recv_exact(c, ln - 4)",
        "        if not body: return",
        "        log.write(b'startup\\n')",
        "        if mode == 'cleartext':",
        "            send(c, b'R', struct.pack('!I', 3))",
        "        else:",
        "            send(c, b'R', struct.pack('!I', 5) + bytes([0xde, 0xad, 0xbe, 0xef]))",
        "        typ = recv_exact(c, 1)",
        "        if typ is None: return",
        "        ln2 = struct.unpack('!I', recv_exact(c, 4))[0]",
        "        pw = recv_exact(c, ln2 - 4)",
        "        log.write(b'password ' + typ + b' ' + pw.hex().encode() + b'\\n')",
        "        send(c, b'E', b'SFATAL\\x00C28P01\\x00Mpassword authentication failed\\x00\\x00')",
        "        send(c, b'Z', b'I')",
        "    except Exception as e:",
        "        log.write(b'exc ' + str(e)[:60].encode() + b'\\n')",
        "    c.close()",
        "while True:",
        "    c, _ = srv.accept()",
        "    threading.Thread(target=handle, args=(c,), daemon=True).start()",
    ].join("\n"));
    sh(`python3 ${P("fakepg.py")} ${P("port")} ${P("pglog")} md5 >${P("srv.log")} 2>&1 &`);
    let port = 0;
    for (let i = 0; i < 80 && !port; i++) {
        sh("sleep 0.1");
        try { port = parseInt(readFile(new Path(P("port"))), 10) || 0; } catch (e) {}
    }
    if (!port) {
        ok(false, "fake PG server did not start");
        return;
    }
    const log = () => { try { return readFile(new Path(P("pglog"))); } catch (e) { return ""; } };

    const client = new PostgreSQL({
        host: "127.0.0.1", port, user: "", password: "",
        database: "x", insecureAuth: true, raw: true,
    });
    const t0 = Date.now();
    let rejected = false, msg = "";
    try {
        await client.query("select 1");
    } catch (e) {
        rejected = true;
        msg = String(e);
    }
    const dt = Date.now() - t0;
    ok(rejected, "short credentials: the MD5-auth attempt completes without a crash and rejects");
    ok(dt < 5000, "the failure is prompt (dt=" + dt + "ms)");
    for (let i = 0; i < 40 && log().indexOf("password") === -1; i++)
        sh("sleep 0.1");
    const l = log();
    ok(l.indexOf("password p ") !== -1,
        "the client sent a PasswordMessage ('p') to the MD5 challenge", JSON.stringify(l.slice(0, 120)));
    ok(/password p 6d6435[0-9a-f]{32}/.test(l),
        "the MD5 password message is md5 + 32 hex digits (36 bytes)", JSON.stringify(l.slice(0, 120)));
    client.close();

    const c2 = new PostgreSQL({
        host: "127.0.0.1", port: 1, user: "u", password: "",
        database: "x", insecureAuth: true, raw: true,
    });
    ok(c2 !== undefined, "a second client with an empty password constructs");
    c2.close();
}

main().catch((e) => {
    fail++;
    console.log("  FAIL harness error " + String(e).slice(0, 120));
}).then(() => {
    console.log("test_net_pg_auth_hostile: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
    if (fail)
        throw new Error("test_net_pg_auth_hostile: " + fail + " failures");
});
