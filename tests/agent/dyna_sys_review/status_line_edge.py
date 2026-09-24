#!/usr/bin/env python3
"""status_line_edge.py — INDEPENDENT probe for the residual edge of FIX 4:
a status line that starts with HTTP/ but carries no status code
("HTTP/1.1\\r\\n") — code-reading says it still fabricates {status:0}.
Runs the engine against a one-shot local server on 127.0.0.1:0.
"""
import os, socket, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
OUT = os.path.join(HERE, "out")
os.makedirs(OUT, exist_ok=True)

RAW = sys.argv[1] if len(sys.argv) > 1 else "HTTP/1.1\r\n"
LABEL = sys.argv[2] if len(sys.argv) > 2 else "bare"

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 0))
srv.listen(1)
port = srv.getsockname()[1]

def serve():
    try:
        c, _ = srv.accept()
        c.recv(65536)
        c.sendall(RAW.encode("latin-1") + b"\r\n\r\n")
        time.sleep(0.3)
        c.close()
    except Exception:
        pass

t = threading.Thread(target=serve, daemon=True)
t.start()

script = os.path.join(OUT, "status_line_edge.js")
with open(script, "w") as f:
    f.write('''
import { HTTPClient } from "dyna:net";
import { getEnv } from "dyna:sys";
var port = parseInt(getEnv("EDGE_PORT"));
var c = new HTTPClient();
c.getAsync("http://127.0.0.1:" + port + "/x").then(
  function (r) { print("RESOLVED status=" + r.status + " body=" + String(r.body).slice(0, 30)); },
  function (e) { print("REJECTED " + (e && (e.dynajsError !== undefined ? "dynajsError=" + e.dynajsError : String(e)))); });
setTimeout(function () {}, 400);
''')

for name, binp in (("pristine", os.path.join(ROOT, "dynajs.pristine")),
                   ("patched", os.path.join(ROOT, "dynajs"))):
    t2 = threading.Thread(target=serve, daemon=True)
    # re-listen: single-shot server consumed above; restart
    t2 = threading.Thread(target=serve, daemon=True)
    t2.start()
    env = dict(os.environ)
    env["EDGE_PORT"] = str(port)
    p = subprocess.run(["timeout", "10", binp, script], cwd=ROOT, env=env,
                       capture_output=True, text=True, timeout=15)
    print("%-8s %-9s rc=%d %s" % (LABEL, name, p.returncode,
                                  (p.stdout or p.stderr).strip().replace("\n", "|")[:100]))
srv.close()
