#!/usr/bin/env python3
"""exit_taxonomy.py — INDEPENDENT exit-code taxonomy for the FIX 8 review.

Runs the ENGINE as a subprocess for each uncaught-throw class and records
(rc, ticks, output) for BOTH binaries: dynajs (patched) and dynajs.pristine.
FIX 8 intentionally changes the patched column (timer-callback throws now
exit 1); the pristine column documents the old semantics.

Safety: 127.0.0.1 ephemeral ports only; scripts and outputs live inside
tests/agent/dyna_sys_review/out/tax/; every subprocess has a timeout.
"""
import os, socket, subprocess, sys, textwrap, time, json

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
OUT = os.path.join(HERE, "out", "tax")
os.makedirs(OUT, exist_ok=True)
PATCHED = os.path.join(ROOT, "dynajs")
PRISTINE = os.path.join(ROOT, "dynajs.pristine")

# ---- local tcp sink on an ephemeral port (for the net-callback class) ----
def sink_server():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    s.listen(8)
    port = s.getsockname()[1]
    import threading
    def serve():
        try:
            while True:
                c, _ = s.accept()
                c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi")
                import time as _t; _t.sleep(0.2)
                c.close()
        except Exception:
            pass
    threading.Thread(target=serve, daemon=True).start()
    return s, port

CASES = [
    # name, script, needs_sink
    ("top_throw", 'throw new Error("top");\n', False),
    ("timer_throw",
     'print("armed");\nsetTimeout(function () { throw new Error("timer"); }, 30);\n', False),
    ("interval_throw",
     'var i = 0;\nsetInterval(function () { i++; print("tick" + i); throw new Error("iv"); }, 30);\n', False),
    ("nested_timer_throw",
     'setTimeout(function () { setTimeout(function () { throw new Error("n"); }, 20); }, 20);\n', False),
    ("timer_throw_after_success",
     'print("before");\nsetTimeout(function () { throw new Error("late"); }, 30);\n', False),
    ("microtask_after_timer_throw",
     'setTimeout(function () { Promise.resolve().then(function () { throw new Error("m"); }); }, 20);\n', False),
    ("promise_reject_unhandled",
     'Promise.reject(new Error("r"));\nsetTimeout(function () {}, 60);\n', False),
    ("async_fn_throw_unhandled",
     '(async function () { throw new Error("a"); })();\nsetTimeout(function () {}, 60);\n', False),
    ("timer_throw_caught",
     'setTimeout(function () { try { throw new Error("c"); } catch (e) { print("caught"); } }, 30);\n', False),
    ("interval_self_clear",
     'var n = 0;\nvar id = setInterval(function () { n++; if (n >= 3) { clearInterval(id); print("done " + n); } }, 20);\n', False),
    ("net_data_handler_throw", r'''
import { TCPServer } from "dyna:net";
import { getEnv } from "dyna:sys";
var port = parseInt(getEnv("DYN_TAX_PORT"));
var srv = new TCPServer({ port: port, handlers: {
  connect: function (c) { print("conn"); },
  data: function (c, b) { throw new Error("data-handler"); },
}});
srv.start();
setTimeout(function () { print("alive-after-throw"); srv.close(); }, 300);
''', True),
    ("udp_message_handler_throw", r'''
import { UDPSocket } from "dyna:net";
import { getEnv } from "dyna:sys";
var port = parseInt(getEnv("DYN_TAX_PORT"));
var rx = new UDPSocket({ port: 0, host: "127.0.0.1" });
rx.start({ message: function (d, f) { throw new Error("udp-handler"); } });
var tx = new UDPSocket({ port: 0, host: "127.0.0.1" });
tx.send("x", "127.0.0.1", rx.port);
setTimeout(function () { print("alive-after-udp-throw"); rx.close(); tx.close(); }, 300);
''', True),
    ("file_async_rejection_unhandled", r'''
import { readFileAsync } from "dyna:file";
readFileAsync("tests/agent/dyna_sys_review/out/tax/definitely-missing.bin").then(function (x) { print("no"); });
setTimeout(function () {}, 100);
''', False),
    ("http_async_reject_unhandled", r'''
import { HTTPClient } from "dyna:net";
import { getEnv } from "dyna:sys";
var port = parseInt(getEnv("DYN_TAX_CLOSED_PORT"));
var c = new HTTPClient();
c.getAsync("http://127.0.0.1:" + port + "/x");
setTimeout(function () { print("after-http-reject"); }, 400);
''', True),
    ("std_exit_3", 'import { exit } from "std";\nprint("bye");\nexit(3);\n', False),
    ("emfile_code", r'''
import { Path, FileReader } from "dyna:file";
var held = [];
var code = "none";
try {
  for (var i = 0; i < 400; i++) held.push(new FileReader(new Path("tests/agent/dyna_sys_review/out/tax/target.md")));
} catch (e) { code = String(e.code || e); }
print("EMFILE-CODE:" + code);
''', False),
]

def _low_nofile():
    import resource
    resource.setrlimit(resource.RLIMIT_NOFILE, (40, 40))

def run_repl_case(binpath, sink, port, closed_port, tmo=15):
    """uncaught throw typed at the REPL: dump-and-continue, then EOF -> rc?"""
    env = dict(os.environ)
    env["DYN_TAX_PORT"] = str(port)
    env["DYN_TAX_CLOSED_PORT"] = str(closed_port)
    try:
        p = subprocess.run(["timeout", str(tmo), binpath], cwd=ROOT, env=env,
                           input="var a = 1;\nthrow new Error('repl-throw');\nprint('still-alive', a);\n",
                           capture_output=True, text=True, timeout=tmo + 5)
        return {"name": "repl_throw", "engine": os.path.basename(binpath),
                "rc": p.returncode, "out": p.stdout[-300:], "err": p.stderr[-200:],
                "secs": 0, "ticks": 0}
    except subprocess.TimeoutExpired:
        return {"name": "repl_throw", "engine": os.path.basename(binpath),
                "rc": 124, "out": "", "err": "timeout", "secs": tmo, "ticks": 0}

def run_case(binpath, name, script, needs_sink, sink, port, closed_port=0, tmo=12):
    path = os.path.join(OUT, name + ".js")
    with open(path, "w") as f:
        f.write(script)
    env = dict(os.environ)
    env["DYN_TAX_PORT"] = str(port)
    env["DYN_TAX_CLOSED_PORT"] = str(closed_port)
    t0 = time.time()
    try:
        pre = _low_nofile if name == "emfile_code" else None
        p = subprocess.run(["timeout", str(tmo), binpath, path], cwd=ROOT,
                           env=env, capture_output=True, text=True, timeout=tmo + 5,
                           preexec_fn=pre)
        rc, out, err = p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        rc, out, err = 124, "", "driver-timeout"
    dt = time.time() - t0
    return {"name": name, "engine": os.path.basename(binpath), "rc": rc,
            "out": out[-800:], "err": err[-400:], "secs": round(dt, 2)}

def main():
    sink, port = sink_server()
    dead = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    dead.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    dead.bind(("127.0.0.1", 0))
    closed_port = dead.getsockname()[1]
    dead.close()
    rows = []
    for name, script, needs_sink in CASES:
        for binpath in (PRISTINE, PATCHED):
            r = run_case(binpath, name, script, needs_sink, sink, port, closed_port)
            r["ticks"] = r["out"].count("tick")
            rows.append(r)
            print("%-32s %-9s rc=%-4d %s" % (name, r["engine"], r["rc"],
                                             r["out"].replace("\n", "|")[-60:]))
    for binpath in (PRISTINE, PATCHED):
        r = run_repl_case(binpath, sink, port, closed_port)
        rows.append(r)
        print("%-32s %-9s rc=%-4d %s" % ("repl_throw", r["engine"], r["rc"],
                                         r["out"].replace("\n", "|")[-60:]))
    with open(os.path.join(OUT, "taxonomy.json"), "w") as f:
        json.dump(rows, f, indent=1)
    sink.close()
    print("wrote", os.path.join(OUT, "taxonomy.json"))

if __name__ == "__main__":
    main()
