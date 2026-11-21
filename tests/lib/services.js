// tests/lib/services.js -- external servers a suite needs, closed by the suite
// that opened them (owner rule 15: tests close every external resource they open).
//
//   const r = await ensureRedis();              // { ok, ours, port } on REDIS_PORT 56379
//   const p = await ensurePostgres({ database: "djtest", tls: true });
//       // { ok, ours, port, conn, trust, ca } -- conn: scram login (postgres/pw), trust:
//       // the password-less role djtrust, ca: the PEM that signs the server's TLS cert
//   ... cases ...
//   await stopStarted();                        // t.after(...) / after __kitS.end()
//
// ensure* reuses a server that already answers on the port (ours = false: never
// stopped here) and otherwise starts one (ours = true). A started server is a
// FOREGROUND child of this process (Spawn, never --daemonize / pg_ctl start), so
// it is in this process's child-group table: if a case throws, the suite exits
// early, or the process is signalled, the runtime's exit hook (see Exec/Spawn
// "Child lifetime" in dynajs.d.ts) SIGTERMs and then SIGKILLs it. stopStarted()
// is the orderly teardown on the normal path: `redis-cli -p P shutdown nosave`,
// `pg_ctl -D DIR -m fast stop` (SIGTERM to the postmaster when pg_ctl is
// missing), then the temporary cluster directory is removed.
// Ports: DYNA_TEST_PG_PORT / DYNA_TEST_REDIS_PORT (default 55432 / 56379); run2.sh gives each concurrent suite its own
// slot pair, so every DB suite starts a private server (per-suite isolation). DYNA_TEST_PG_TEMPLATE=<dir> caches one
// initdb'd cluster there and clones it (APFS cp -c / reflink) instead of running initdb per suite.
// Without those env vars every PostgreSQL suite shares ONE server: 127.0.0.1:55432 (PG_PORT), superuser postgres/pw over
// scram-sha-256, role djtrust over trust, ssl = on with a certificate from testCert() whose PEM is
// <cluster>/tls/cert.pem, so another process reusing the server finds the same CA.
// A started server's command line carries "dj-test-service" (redis: --logfile
// suffix; postgres: the cluster dir name) so a leak check can find any that
// outlive their suite.
import * as std from "std";
import * as os from "os";
import { Spawn, Exec, Which } from "dyna:sys";

const TAG = "dj-test-service";
const TMP = (() => { const t = std.getenv("TMPDIR") || ".obj/svc-tmp"; if (t.startsWith("/")) return t; const [cwd] = os.getcwd(); return `${cwd}/${t}`; })();
const required = (r, kind) => { if (!r.ok && std.getenv("DYNA_TEST_SERVICES_REQUIRED")) print(`FAIL: services.js: ${kind} unavailable under a runner that promised it (DYNA_TEST_SERVICES_REQUIRED): ${r.why}`); return r; };
const started = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const sh = (cmd, env) => Exec("/bin/sh", ["-c", cmd], { timeoutMs: 30000, encoding: "utf8", ...(env ? { env } : {}) });

function tool(name) {
    const w = Which(name);
    if (w) return w;
    const r = sh(`ls -1 /opt/homebrew/bin/${name}-* /usr/local/bin/${name}-* /usr/lib/postgresql/*/bin/${name} 2>/dev/null | sort -V | tail -1`);
    const p = String(r.stdout || "").trim();
    return p || null;
}

function sweepDead() {
    const root = TMP.replace(/\/+$/, "");
    sh('for d in "' + root + '"/' + TAG + '-*; do [ -e "$d" ] || continue; p=${d%.initdb.log}; p=${p##*-}; case "$p" in ""|*[!0-9]*) continue ;; esac; kill -0 "$p" 2>/dev/null || rm -rf "$d"; done');
}

function redisUp(port) {
    return sh(`redis-cli -p ${port} ping 2>/dev/null`).stdout.trim() === "PONG";
}

const envPort = (k, d) => { const v = Number(std.getenv(k) ?? ""); return Number.isInteger(v) && v > 0 && v < 65536 ? v : d; };
export const REDIS_PORT = envPort("DYNA_TEST_REDIS_PORT", 56379);

export async function ensureRedis(port = REDIS_PORT) { return required(await ensureRedis0(port), "redis"); }
async function ensureRedis0(port) {
    sweepDead();
    if (redisUp(port)) return { ok: true, ours: false, port };
    if (!Which("redis-server")) return { ok: false, ours: false, port, why: "redis-server is not installed" };
    const dir = `${TMP}/${TAG}-redis-${port}-${os.getpid()}`;
    sh(`mkdir -p '${dir}'`);
    const child = new Spawn("/bin/sh", ["-c", `exec redis-server --port ${port} --bind 127.0.0.1 --save '' --appendonly no --dir '${dir}' --logfile '${dir}/redis.log'`]);
    for (let i = 0; i < 100 && !redisUp(port); i++) await sleep(20);
    if (!redisUp(port)) {
        try { child.kill(); } catch (e) {}
        return { ok: false, ours: false, port, why: "redis-server did not answer on " + port };
    }
    started.push({ kind: "redis", port, child, dir });
    return { ok: true, ours: true, port };
}

function tcpOpen(port) {
    return Exec("/bin/bash", ["-c", `exec 3<>/dev/tcp/127.0.0.1/${port}`], { timeoutMs: 5000, encoding: "utf8" }).code === 0;
}

export const PG_PORT = envPort("DYNA_TEST_PG_PORT", 55432), PG_USER = "postgres", PG_PASSWORD = "pw", PG_TRUST_USER = "djtrust";

function pgResult(port, user, password, database, ours, ca) {
    const host = "127.0.0.1";
    return { ok: true, ours, port, ca, conn: { host, port, user, password, database }, trust: { host, port, user: PG_TRUST_USER, database } };
}

function pgSql(bin, port, user, password, sql) {
    return sh(`'${bin}/psql' -h 127.0.0.1 -p ${port} -U '${user}' -d postgres -v ON_ERROR_STOP=1 -qAt -c "${sql}" 2>&1`, { ...std.getenviron(), PGPASSWORD: password });
}

function pgEnsureDb(bin, port, user, password, database) {
    if (database === "postgres" || !bin) return;
    if (pgSql(bin, port, user, password, `select 1 from pg_database where datname = '${database}'`).stdout.trim() !== "1")
        pgSql(bin, port, user, password, `create database ${database}`);
}

function pgSharedCa(port) {
    const root = TMP.replace(/\/+$/, "");
    const r = sh(`ls -1 "${root}"/${TAG}-pg-${port}-*/tls/cert.pem 2>/dev/null | head -1`);
    return String(r.stdout || "").trim() || null;
}

export async function ensurePostgres(o = {}) { return required(await ensurePostgres0(o), "postgres"); }
async function ensurePostgres0({ port = PG_PORT, user = PG_USER, password = PG_PASSWORD, database = "postgres", tls = false } = {}) {
    sweepDead();
    if (tcpOpen(port)) {
        const ca = pgSharedCa(port);
        if (tls && !ca) return { ok: false, ours: false, port, why: "the PostgreSQL on " + port + " was not started by tests/lib/services.js, so it has no TLS CA here" };
        const initdb = tool("initdb");
        pgEnsureDb(initdb ? String(os.realpath(initdb)[0] || initdb).replace(/\/[^/]*$/, "") : null, port, user, password, database);
        return pgResult(port, user, password, database, false, ca);
    }
    const initdb = tool("initdb");
    if (!initdb) return { ok: false, ours: false, port, why: "initdb is not installed" };
    const [real] = os.realpath(initdb);
    const bin = String(real || initdb).replace(/\/[^/]*$/, "");
    const postgres = bin + "/postgres";
    const dir = `${TMP}/${TAG}-pg-${port}-${os.getpid()}`;
    sh(`rm -rf '${dir}' && mkdir -p '${dir}' && printf '%s\\n' '${password}' > '${dir}.pw'`);
    const initTo = (d) => sh(`LC_ALL=C '${initdb}' -D '${d}' -U '${user}' --auth=scram-sha-256 --pwfile='${dir}.pw' >'${dir}.initdb.log' 2>&1`);
    const tmplRoot = std.getenv("DYNA_TEST_PG_TEMPLATE");
    let init;
    if (tmplRoot && user === PG_USER && password === PG_PASSWORD) {
        const t = `${tmplRoot}/${bin.replace(/[^A-Za-z0-9.]+/g, "_")}`;
        if (sh(`test -f '${t}/PG_VERSION'`).code !== 0) {
            const b = `${t}.build.${os.getpid()}`;
            sh(`mkdir -p '${tmplRoot}' && rm -rf '${b}'`);
            if (initTo(b).code === 0) sh(`[ -e '${t}' ] || mv '${b}' '${t}'; rm -rf '${b}'`);
        }
        init = sh(`test -f '${t}/PG_VERSION' && { cp -cR '${t}' '${dir}/data' 2>/dev/null || cp -a --reflink=auto '${t}' '${dir}/data'; }`);
        if (init.code !== 0) { sh(`rm -rf '${dir}/data'`); init = initTo(`${dir}/data`); }
    } else init = initTo(`${dir}/data`);
    sh(`rm -f '${dir}.pw'`);
    if (init.code !== 0) { sh(`rm -rf '${dir}' '${dir}.initdb.log'`); return { ok: false, ours: false, port, why: "initdb failed" }; }
    const hba = [`host all ${PG_TRUST_USER} 127.0.0.1/32 trust`, `host all ${PG_TRUST_USER} ::1/128 trust`, "host all all 127.0.0.1/32 scram-sha-256", "host all all ::1/128 scram-sha-256", ""].join("\n");
    { const fh = std.open(`${dir}/data/pg_hba.conf`, "w"); fh.puts(hba); fh.close(); }
    const cert = testCert({ dir: `${dir}/tls` });
    const ssl = cert ? ` -c ssl=on -c ssl_cert_file='${cert.cert}' -c ssl_key_file='${cert.key}'` : "";
    const child = new Spawn("/bin/sh", ["-c", `LC_ALL=C exec '${postgres}' -D '${dir}/data' -p ${port} -c unix_socket_directories='' -c listen_addresses=127.0.0.1 -c fsync=off${ssl} >'${dir}/postgres.log' 2>&1`]);
    const entry = { kind: "postgres", port, child, dir, bin };
    started.push(entry);
    for (let i = 0; i < 250 && !tcpOpen(port); i++) await sleep(20);
    if (!tcpOpen(port)) { if (std.getenv("SVC_DEBUG")) print(std.loadFile(dir + "/postgres.log")); await stopOne(entry); started.splice(started.indexOf(entry), 1); return { ok: false, ours: false, port, why: "postgres did not listen on " + port }; }
    let ready = false;
    for (let i = 0; i < 250 && !ready; i++) { ready = pgSql(bin, port, user, password, "select 1").stdout.trim() === "1"; if (!ready) await sleep(20); }
    pgSql(bin, port, user, password, `create role ${PG_TRUST_USER} login superuser`);
    pgEnsureDb(bin, port, user, password, database);
    return pgResult(port, user, password, database, true, cert ? cert.ca : null);
}

async function stopOne(s) {
    if (s.kind === "cert") { sh("rm -rf '" + s.dir + "'"); return; }
    if (s.kind === "exim" || s.kind === "mailpit") try { s.child.kill("SIGTERM"); } catch (e) {}
    else if (s.kind === "redis") sh(`redis-cli -p ${s.port} shutdown nosave >/dev/null 2>&1`);
    else {
        const pgctl = s.bin + "/pg_ctl";
        if (sh(`test -x '${pgctl}'`).code === 0) sh(`'${pgctl}' -D '${s.dir}/data' -m fast -w -t 10 stop >/dev/null 2>&1`);
        else try { s.child.kill("SIGTERM"); } catch (e) {}
    }
    const done = s.child.wait();
    const t = await Promise.race([done.then(() => true), sleep(5000).then(() => false)]);
    if (!t) { try { s.child.kill("SIGKILL"); } catch (e) {} await done; }
    sh(`rm -rf '${s.dir}' '${s.dir}.initdb.log'`);
}

export async function stopStarted() {
    while (started.length) await stopOne(started.pop());
}

export function startedServices() {
    return started.map((s) => s.kind + ":" + s.port);
}

function eximConf(dir, port, user, group, extra) {
    return [
        ...extra,
        `exim_user = ${user}`, `exim_group = ${group}`, "never_users =", "primary_hostname = mx.test",
        `spool_directory = ${dir}/spool`, `log_file_path = ${dir}/log/%slog`, `pid_file_path = ${dir}/exim.pid`,
        `daemon_smtp_ports = ${port}`, "local_interfaces = 127.0.0.1", "domainlist local_domains = test.example",
        "acl_smtp_rcpt = acl_rcpt", "acl_smtp_data = acl_data", "tls_advertise_hosts = *",
        `tls_certificate = ${dir}/cert.pem`, `tls_privatekey = ${dir}/key.pem`, "chunking_advertise_hosts = *",
        "pipelining_advertise_hosts = *", "accept_8bitmime = true", "dsn_advertise_hosts = *",
        "message_size_limit = 1M", "queue_only = true", "smtp_accept_max = 64", "smtp_receive_timeout = 30s",
        "begin acl", "acl_rcpt:", "  deny local_parts = nobody", "  accept domains = +local_domains",
        "  deny message = relay not permitted", "acl_data:", "  accept",
        "begin routers", "begin transports", "begin authenticators",
        "plain:", "  driver = plaintext", "  public_name = PLAIN", "  server_prompts = :",
        "  server_condition = ${if and {{eq{$auth2}{tim}}{eq{$auth3}{pw}}}}", "  server_set_id = $auth2",
        "login:", "  driver = plaintext", "  public_name = LOGIN", "  server_prompts = Username:: : Password::",
        "  server_condition = ${if and {{eq{$auth1}{tim}}{eq{$auth2}{pw}}}}", "  server_set_id = $auth1", "",
    ].join("\n");
}

// exim on 127.0.0.1:port as a FOREGROUND child (-bdf), temp spool/log/config/self-signed cert under
// dir; extra = main-section config lines placed first (e.g. "smtp_accept_max_per_connection = 3"); it queues (queue_only) and never delivers or relays: only *@test.example is accepted.
// PLAIN/LOGIN credentials tim/pw. { ok, port, dir, ca } -- read messages from dir/spool/input/*-D.
export async function ensureExim(port, extra = []) {
    sweepDead();
    const exim = tool("exim");
    if (!exim) return { ok: false, ours: false, port, why: "exim is not installed" };
    if (tcpOpen(port)) return { ok: false, ours: false, port, why: "port " + port + " is busy" };
    const dir = `${TMP}/${TAG}-exim-${port}-${os.getpid()}`.replace(/\/+/g, "/");
    sh(`rm -rf '${dir}' && mkdir -p '${dir}/spool' '${dir}/log'`);
    const cert = sh(`openssl req -x509 -newkey rsa:2048 -nodes -keyout '${dir}/key.pem' -out '${dir}/cert.pem' -days 2 -subj /CN=localhost -addext subjectAltName=DNS:localhost,IP:127.0.0.1 >/dev/null 2>&1`);
    if (cert.code !== 0) { sh(`rm -rf '${dir}'`); return { ok: false, ours: false, port, why: "openssl could not make a test certificate" }; }
    const user = sh("id -un").stdout.trim(), group = sh("id -gn").stdout.trim();
    const f = std.open(`${dir}/exim.conf`, "w");
    f.puts(eximConf(dir, port, user, group, extra));
    f.close();
    const child = new Spawn("/bin/sh", ["-c", `exec '${exim}' -C '${dir}/exim.conf' -bdf -oX ${port} >'${dir}/daemon.log' 2>&1`]);
    const entry = { kind: "exim", port, child, dir };
    started.push(entry);
    for (let i = 0; i < 250 && !tcpOpen(port); i++) await sleep(20);
    if (!tcpOpen(port)) { await stopOne(entry); started.splice(started.indexOf(entry), 1); return { ok: false, ours: false, port, why: "exim did not listen on " + port }; }
    return { ok: true, ours: true, port, dir, ca: `${dir}/cert.pem` };
}

// mailpit SMTP on smtpPort and its HTTP API on httpPort, temp database, AUTH accepted for any
// credentials (also without TLS). Raw message: GET /api/v1/message/<ID>/raw.
export async function ensureMailpit(smtpPort, httpPort) {
    sweepDead();
    const mp = tool("mailpit");
    if (!mp) return { ok: false, ours: false, port: smtpPort, why: "mailpit is not installed" };
    if (tcpOpen(smtpPort) || tcpOpen(httpPort)) return { ok: false, ours: false, port: smtpPort, why: "mailpit ports are busy" };
    const dir = `${TMP}/${TAG}-mailpit-${smtpPort}-${os.getpid()}`.replace(/\/+/g, "/");
    sh(`rm -rf '${dir}' && mkdir -p '${dir}'`);
    const child = new Spawn("/bin/sh", ["-c", `exec '${mp}' -s 127.0.0.1:${smtpPort} -l 127.0.0.1:${httpPort} -d '${dir}/mp.db' --smtp-auth-accept-any --smtp-auth-allow-insecure --max 100000 >'${dir}/mailpit.log' 2>&1`]);
    const entry = { kind: "mailpit", port: smtpPort, child, dir };
    started.push(entry);
    for (let i = 0; i < 250 && !(tcpOpen(smtpPort) && tcpOpen(httpPort)); i++) await sleep(20);
    if (!tcpOpen(smtpPort)) { await stopOne(entry); started.splice(started.indexOf(entry), 1); return { ok: false, ours: false, port: smtpPort, why: "mailpit did not listen on " + smtpPort }; }
    return { ok: true, ours: true, port: smtpPort, http: httpPort, dir };
}

// Self-signed localhost certificate (CN and SAN localhost, 127.0.0.1), its own CA: { cert, key, ca }
// PEM paths (key mode 0600), or null when openssl is missing. Without opts.dir it lives in a temp dir
// removed by stopStarted(); with opts.dir the caller owns that directory.
// opts.alg "ed25519": Ed25519 key, whose signature has no hash (RFC 5929 end-point hash unusable); default RSA 2048.
export function testCert(opts = {}) {
    sweepDead();
    const dir = (opts.dir || `${TMP}/${TAG}-cert-${Math.random().toString(36).slice(2, 8)}-${os.getpid()}`).replace(/\/+/g, "/");
    const r = sh(`mkdir -p '${dir}' && openssl req -x509 -newkey ${opts.alg === "ed25519" ? "ed25519" : "rsa:2048"} -nodes -keyout '${dir}/key.pem' -out '${dir}/cert.pem' -days 2 -subj /CN=localhost -addext subjectAltName=DNS:localhost,IP:127.0.0.1 >/dev/null 2>&1 && chmod 600 '${dir}/key.pem'`);
    if (r.code !== 0) { sh(`rm -rf '${dir}'`); return null; }
    if (!opts.dir) started.push({ kind: "cert", port: 0, dir, child: null });
    return { dir, cert: `${dir}/cert.pem`, key: `${dir}/key.pem`, ca: `${dir}/cert.pem` };
}
