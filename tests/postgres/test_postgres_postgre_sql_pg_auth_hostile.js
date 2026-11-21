// flags: --std
// timeout: 120
// covers: dyna:postgres.PostgreSQL
// slots: 2
import { sections as __kitSections } from "../lib/kit.js";
import { PostgreSQL } from "dyna:postgres";
import { Exec, Which } from "dyna:sys";
import { makeTempDir, writeFile, readFile, Path } from "dyna:file";
import { spawnHelper, stopHelpers } from "../lib/helper.js";
const __kitS = __kitSections("test_postgres_postgre_sql_pg_auth_hostile", { chain: true });

let pass = 0, fail = 0, skip = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; console.log("  FAIL " + w + (d ? "  [" + d + "]" : "")); } };

async function main() {
let T, client, t0, dt, l, c2;
    const P = (n) => T + "/" + n;
    const sh = (c) => Exec("/bin/sh", ["-c", c]).code;
    let port = 0;
    const log = () => { try { return readFile(new Path(P("pglog"))); } catch (e) { return ""; } };
__kitS.section("fake PG server did not start", () => {
    if (!Which("python3")) {
        skip++;
        console.log("test_net_pg_auth_hostile: python3 missing, fake server skipped");
        return;
    }
    T = makeTempDir("d2pgauth");

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
        "import os, time",
        "ppid = os.getppid()",
        "def orphaned():",
        "    while os.getppid() == ppid: time.sleep(0.5)",
        "    os._exit(0)",
        "threading.Thread(target=orphaned, daemon=True).start()",
        "srv.settimeout(30)",
        "while True:",
        "    try: c, _ = srv.accept()",
        "    except socket.timeout: os._exit(0)",
        "    threading.Thread(target=handle, args=(c,), daemon=True).start()",
    ].join("\n"));
    spawnHelper(`python3 ${P("fakepg.py")} ${P("port")} ${P("pglog")} md5 >${P("srv.log")} 2>&1`);
    for (let i = 0; i < 80 && !port; i++) {
        sh("sleep 0.1");
        try { port = parseInt(readFile(new Path(P("port"))), 10) || 0; } catch (e) {}
    }
    if (!port) {
        ok(false, "fake PG server did not start");
        return;
    }
});
    let rejected = false, msg = "";

await __kitS.section("short credentials: the MD5-auth attempt completes without a crash and rejects", async () => {
    client = new PostgreSQL({
        host: "127.0.0.1", port, user: "", password: "",
        database: "x", insecureAuth: true, raw: true,
    });
    t0 = Date.now();
    try {
        await client.query("select 1");
    } catch (e) {
        rejected = true;
        msg = String(e);
    }
    dt = Date.now() - t0;
    ok(rejected, "short credentials: the MD5-auth attempt completes without a crash and rejects");
});
__kitS.section("the auth failure is prompt, not a hang", () => {
    // wall-clock: a deadline/timeout check; the bound sits far above the deadline and below the hang it guards against, so load only narrows the gap
    ok(dt < 15000, "the failure is prompt, not a hang (dt=" + dt + "ms)");
});
__kitS.section("the client sent a PasswordMessage ('p') to the MD5 challenge", () => {
    for (let i = 0; i < 40 && log().indexOf("password") === -1; i++)
        sh("sleep 0.1");
    l = log();
    ok(l.indexOf("password p ") !== -1,
        "the client sent a PasswordMessage ('p') to the MD5 challenge", JSON.stringify(l.slice(0, 120)));
    ok(/password p 6d6435[0-9a-f]{32}/.test(l),
        "the MD5 password message is md5 + 32 hex digits (36 bytes)", JSON.stringify(l.slice(0, 120)));
});
__kitS.section("a second client with an empty password constructs", () => {
    client.close();

    c2 = new PostgreSQL({
        host: "127.0.0.1", port: 1, user: "u", password: "",
        database: "x", insecureAuth: true, raw: true,
    });
    ok(c2 !== undefined, "a second client with an empty password constructs");
    c2.close();
});
}

await main().catch((e) => {
    fail++;
    console.log("  FAIL harness error " + String(e).slice(0, 120));
}).then(() => {
    stopHelpers();
    console.log("test_net_pg_auth_hostile: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
    if (fail)
        throw new Error("test_net_pg_auth_hostile: " + fail + " failures");
});
__kitS.end();
