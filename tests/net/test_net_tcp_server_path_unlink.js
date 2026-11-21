// flags: --std
// timeout: 60
// covers: dyna:net.TCPServer -- a unix-path TCPServer unlinks only the socket inode it bound; start() never deletes a non-socket file or a live socket, and reclaims only a stale socket
// Security fix: dispose unlinked t->path even for a server that never listened (new TCPServer({ path }) then close or GC
// deleted any file at that path), and dyn_aio_unix_listen unlinked whatever was at the path before bind (a regular
// file, or another process's live socket, which it then hijacked). Now start() fails (listen failed) unless the path is
// absent or a stale socket (connect gives ECONNREFUSED), and close unlinks only when the path still names the inode
// this server bound. Oracle: lstat/connect results on a private short dir under /tmp.
import { TCPServer } from "dyna:net";
import * as std from "std";
import * as os from "os";

let n = 0, fails = 0;
const check = (ok, m) => { n++; if (!ok) { fails++; print("FAIL: " + m); } };
const dir = "/tmp/djpu_" + Math.random().toString(36).slice(2, 10);
os.mkdir(dir, 0o700);
const P = (k) => dir + "/" + k;
const st = (p) => { const [s, e] = os.lstat(p); return e === 0 ? s : null; };
const isSock = (p) => { const s = st(p); return !!s && (s.mode & os.S_IFMT) === os.S_IFSOCK; };
const isFile = (p) => { const s = st(p); return !!s && (s.mode & os.S_IFMT) === os.S_IFREG; };
const put = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };
const body = (p) => std.loadFile(p);
const throws = (f) => { try { f(); return null; } catch (e) { return e; } };
const sleep = (ms) => new Promise((r) => os.setTimeout(r, ms));
const connects = (p) => new Promise((res) => {
    let done = false;
    const fin = (v) => { if (!done) { done = true; res(v); } };
    try { TCPServer.connect({ path: p }, { connect(c, err) { fin(!err); try { c.close(); } catch (e) {} }, error() { fin(false); } }); } catch (e) { fin(false); }
    os.setTimeout(() => fin(false), 1000);
});

{
    put(P("a"), "keep-a");
    const s = new TCPServer({ path: P("a") });
    s.close();
    check(isFile(P("a")) && body(P("a")) === "keep-a", "never-started server closed: the file at its path survives");
    put(P("b"), "keep-b");
    let g = new TCPServer({ path: P("b") });
    g = null;
    std.gc();
    check(isFile(P("b")) && body(P("b")) === "keep-b", "never-started server collected: the file at its path survives");
}
{
    const s = new TCPServer({ path: P("c") });
    s.start({});
    check(isSock(P("c")), "start() binds a socket");
    s.close();
    check(!st(P("c")), "close() unlinks the socket it bound");
}
{
    put(P("d"), "keep-d");
    const s = new TCPServer({ path: P("d") });
    const e = throws(() => s.start({}));
    check(e && /listen failed/.test(String(e.message)), "start() over a regular file fails: " + e);
    s.close();
    check(isFile(P("d")) && body(P("d")) === "keep-d", "start() over a regular file leaves it");
}
{
    const live = new TCPServer({ path: P("e") });
    live.start({});
    const s = new TCPServer({ path: P("e") });
    const e = throws(() => s.start({}));
    check(e && /listen failed/.test(String(e.message)), "start() over a live socket fails: " + e);
    s.close();
    check(isSock(P("e")) && await connects(P("e")), "the live socket still accepts after the refused start and close");
    live.close();
    check(!st(P("e")), "the owner's close unlinks it");
}
{
    const s = new TCPServer({ path: P("f") });
    s.start({});
    os.rename(P("f"), P("g"));
    const o = new TCPServer({ path: P("f") });
    o.start({});
    s.close();
    check(isSock(P("f")) && await connects(P("f")), "close() leaves another server's socket now at the bound path");
    o.close();
    check(!st(P("f")), "that server's own close unlinks it");
    put(P("f"), "keep-f");
    check(isSock(P("g")) && !(await connects(P("g"))), "the renamed socket is stale");
    const r = new TCPServer({ path: P("g") });
    const e = throws(() => r.start({}));
    check(e === null && await connects(P("g")), "start() reclaims a stale socket: " + e);
    r.close();
    check(!st(P("g")), "and unlinks it on close");
}
for (const k of ["a", "b", "c", "d", "e", "f", "g"]) os.remove(P(k));
os.remove(dir);
await sleep(10);
print("net_tcp_server_path_unlink: " + n + " checks, " + fails + " failed");
if (fails) std.exit(1);
