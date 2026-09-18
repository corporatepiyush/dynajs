#!/usr/bin/env python3
"""ctl.py — control server for dyna_sys black-box probes.

Modes: http | resp | tcp | udp | tls-client-echo

Safety contract (lane rules): binds 127.0.0.1 ONLY, picks an ephemeral port,
writes the bound port to --portfile, and exits on SIGTERM or stdin EOF (so a
crashed probe can never orphan a listener). All scenarios bounded in time.

The server is the ORACLE: it knows exactly which bytes it sent/received and
exposes them to the probe (headers it received echoed as JSON bodies; raw
bytes seen via the RESP RAWLOG command; per-connection log of events).
"""
import argparse
import gzip
import json
import os
import selectors
import socket
import struct
import sys
import threading
import time

HOST = "127.0.0.1"

def die(msg):
    sys.stderr.write("ctl: " + msg + "\n")
    sys.exit(3)

def bind(portfile, kind="tcp"):
    stype = socket.SOCK_DGRAM if kind == "udp" else socket.SOCK_STREAM
    s = socket.socket(socket.AF_INET, stype)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, 0))
    if kind == "tcp":
        s.listen(64)
    if portfile:
        with open(portfile, "w") as f:
            f.write(str(s.getsockname()[1]) + "\n")
    return s

def watch_stdin():
    # interactive fallback: exit when stdin closes
    try:
        while True:
            b = sys.stdin.buffer.read(4096)
            if not b:
                os._exit(0)
    except Exception:
        os._exit(0)

def watch_parent(ppid):
    # exit when the runner that spawned us goes away (orphaned -> ppid==1) --
    # no orphan listeners even if the runner is SIGKILLed
    import time as _t
    try:
        while True:
            if os.getppid() == 1:
                os._exit(0)
            _t.sleep(0.25)
    except Exception:
        os._exit(0)

# ---------------------------------------------------------------- HTTP mode
def one_mb_body():
    # deterministic: line i = "%06d:abcdefgh\n" so slices are assertable
    out = bytearray()
    line = b""
    i = 0
    while len(out) < 1024 * 1024:
        line = ("%06d:abcdefghijklmnopqrstuvwxyz\n" % i).encode()
        out += line
        i += 1
    return bytes(out[: 1024 * 1024])

ONE_MB = None

class HTTP(threading.Thread):
    def __init__(self, sock):
        super().__init__(daemon=True)
        self.sock = sock

    def run(self):
        self.sock.settimeout(10)
        try:
            while True:
                try:
                    conn, _ = self.sock.accept()
                except OSError:
                    break
                t = threading.Thread(target=self.serve, args=(conn,), daemon=True)
                t.start()
        finally:
            try:
                self.sock.close()
            except Exception:
                pass

    def read_request(self, conn):
        conn.settimeout(10)
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = conn.recv(65536)
            if not chunk:
                return None
            data += chunk
            if len(data) > 8 * 1024 * 1024:
                return None
        head, _, rest = data.partition(b"\r\n\r\n")
        lines = head.split(b"\r\n")
        reqline = lines[0].decode("latin1")
        parts = reqline.split(" ")
        method = parts[0] if parts else ""
        path = parts[1] if len(parts) > 1 else ""
        version = parts[2] if len(parts) > 2 else ""
        headers = []
        for ln in lines[1:]:
            if b":" in ln:
                k, _, v = ln.partition(b":")
                headers.append((k.decode("latin1"), v.strip().decode("latin1")))
        clen = 0
        for k, v in headers:
            if k.lower() == "content-length":
                clen = int(v)
        while len(rest) < clen:
            chunk = conn.recv(65536)
            if not chunk:
                break
            rest += chunk
        body = rest[:clen]
        return {"method": method, "path": path, "version": version,
                "headers": headers, "body": body, "rest": rest[clen:]}

    def serve(self, conn):
        try:
            while True:
                req = self.read_request(conn)
                if req is None:
                    break
                keep = self.respond(conn, req)
                if not keep:
                    break
        except Exception:
            pass
        finally:
            try:
                conn.close()
            except Exception:
                pass

    def respond(self, conn, req):
        path = req["path"]
        # strip query
        base = path.split("?")[0]
        q = {}
        if "?" in path:
            for kv in path.split("?")[1].split("&"):
                if "=" in kv:
                    k, _, v = kv.partition("=")
                    q[k] = v
        keep_alive = "close" not in {v.lower().strip() for k, v in req["headers"] if k.lower() == "connection"}
        is_head = req["method"] == "HEAD"

        def send(raw, close=False):
            conn.sendall(raw)
            return not close

        if base == "/echo":
            # oracle: reply with JSON of exactly what we parsed
            payload = json.dumps({
                "method": req["method"], "path": req["path"],
                "version": req["version"],
                "headers": req["headers"],
                "body": req["body"].decode("utf-8", "replace"),
            }).encode()
            head = ("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                    "Content-Length: %d\r\nConnection: %s\r\n\r\n"
                    % (len(payload), "keep-alive" if keep_alive else "close")).encode()
            if is_head:
                return send(head, close=not keep_alive)  # HEAD: headers only
            return send(head + payload, close=not keep_alive)

        if base == "/status":
            code = int(q.get("c", "200"))
            reason = {200: "OK", 201: "Created", 204: "No Content", 301: "Moved Permanently",
                      302: "Found", 307: "Temporary Redirect", 308: "Permanent Redirect",
                      404: "Not Found", 418: "I'm a teapot", 500: "Internal Server Error",
                      503: "Service Unavailable"}.get(code, "Unknown")
            body = b"" if (code == 204 or is_head) else ("status=%d\n" % code).encode()
            head = "HTTP/1.1 %d %s\r\n" % (code, reason)
            if code != 204:
                head += "Content-Length: %d\r\n" % (len(("status=%d\n" % code).encode()))
            head += "X-Status-Marker: s%d\r\n" % code
            head += "Connection: %s\r\n\r\n" % ("keep-alive" if keep_alive else "close")
            return send(head.encode() + body, close=not keep_alive)

        if base == "/headers":
            body = b"hdrs"
            raw = ("HTTP/1.1 200 OK\r\n"
                   "Set-Cookie: a=1; Path=/\r\n"
                   "Set-Cookie: b=2; Path=/\r\n"
                   "X-Multi: one\r\n"
                   "X-Multi: two\r\n"
                   "X-Weird-Case: PreserveMe-123\r\n"
                   "Content-Length: %d\r\n"
                   "Empty-Header:\r\n"
                   "Connection: %s\r\n\r\n" % (len(body), "keep-alive" if keep_alive else "close")).encode()
            return send(raw + body, close=not keep_alive)

        if base == "/chunked":
            parts = [b"hello ", b"chunked ", b"world"]
            raw = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nX-Chunked: yes\r\n\r\n"
            for p in parts:
                raw += b"%x\r\n" % len(p) + p + b"\r\n"
            raw += b"0\r\nX-Trailer: t1\r\n\r\n"
            return send(raw, close=not keep_alive)

        if base == "/chunked-empty":
            raw = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"
            return send(raw, close=not keep_alive)

        if base == "/body/empty":
            raw = b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"
            return send(raw, close=not keep_alive)

        if base == "/body/1mb":
            body = ONE_MB
            import hashlib
            raw = ("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                   "X-Body-Sha256: %s\r\nContent-Length: %d\r\nConnection: %s\r\n\r\n"
                   % (hashlib.sha256(body).hexdigest(), len(body),
                      "keep-alive" if keep_alive else "close")).encode()
            return send(raw + body, close=not keep_alive)

        if base == "/gz":
            import hashlib
            payload = b"the quick brown fox jumps over the lazy dog " * 40
            gz = gzip.compress(payload)
            raw = ("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nX-Gz-Sha: %s\r\nContent-Length: %d\r\nConnection: %s\r\n\r\n"
                   % (hashlib.sha256(gz).hexdigest(), len(gz),
                      "keep-alive" if keep_alive else "close")).encode() + gz
            return send(raw, close=not keep_alive)

        if base == "/redirect":
            n = int(q.get("n", "1"))
            if n <= 0:
                body = b"landed"
                raw = ("HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body)).encode() + body
                return send(raw, close=not keep_alive)
            loc = "/redirect?n=%d" % (n - 1)
            raw = ("HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: 0\r\n\r\n" % loc).encode()
            return send(raw, close=not keep_alive)

        if base == "/redirect-code":
            code = int(q.get("c", "301"))
            n = int(q.get("n", "1"))
            if n <= 0:
                body = b"landed-%d" % code
                raw = ("HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body)).encode() + body
                return send(raw, close=not keep_alive)
            loc = "/redirect-code?c=%d&n=%d" % (code, n - 1)
            raw = ("HTTP/1.1 %d R\r\nLocation: %s\r\nContent-Length: 0\r\n\r\n" % (code, loc)).encode()
            return send(raw, close=not keep_alive)

        if base == "/loop":
            raw = b"HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n"
            return send(raw, close=False)  # infinite loop; client must bound it

        if base == "/slow":
            # 4 chunks, 120ms apart, bounded ~0.5s total
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 40\r\n\r\n")
            for i in range(4):
                time.sleep(0.12)
                conn.sendall(("chunk%d;" % i).encode().ljust(10, b"."))
            return not keep_alive

        if base == "/slowhead":
            # headers then a long pause before body; client timeout must fire
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n")
            time.sleep(5.0)
            conn.sendall(b"0123456789")
            return False

        if base == "/reset":
            # send partial response then forcibly RST
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\npartial")
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                            struct.pack("ii", 1, 0))
            conn.close()
            return False

        if base == "/garbage":
            return send(b"NOT-HTTP AT ALL\r\n\r\n", close=True)

        if base == "/barestatus":
            # version token only: no space, no status code
            return send(b"HTTP/1.1\r\n\r\n", close=True)

        if base == "/nocode":
            # version + space but a non-3-digit status
            return send(b"HTTP/1.1 20x\r\n\r\n", close=True)

        if base == "/http10":
            body = b"old school"
            raw = ("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n" ).encode() + body
            return send(raw, close=True)  # HTTP/1.0: close-delimited

        if base == "/notouch":
            conn.close()
            return False

        if base == "/method-echo":
            return self.respond(conn, dict(req, path="/echo"))

        if base == "/upload":
            body = json.dumps({"recvlen": len(req["body"]),
                               "sha": __import__("hashlib").sha256(req["body"]).hexdigest(),
                               "ct": next((v for k, v in req["headers"] if k.lower() == "content-type"), "")}).encode()
            raw = ("HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n"
                   % len(body)).encode() + body
            return send(raw, close=not keep_alive)

        # default 404
        body = b"no route"
        raw = ("HTTP/1.1 404 Not Found\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"
               % len(body)).encode() + body
        return send(raw, close=True)

# ---------------------------------------------------------------- RESP mode
class RESP(threading.Thread):
    """Minimal RESP2/RESP3 server. Oracle: RAWLOG returns every raw byte the
    client has sent so far (as a bulk string) so probes can assert wire
    format in BOTH directions."""
    def __init__(self, sock):
        super().__init__(daemon=True)
        self.sock = sock
        self.lock = threading.Lock()
        self.rawlog = bytearray()
        self.store = {}

    def run(self):
        self.sock.settimeout(10)
        conns = []
        try:
            while True:
                conn, _ = self.sock.accept()
                t = threading.Thread(target=self.serve, args=(conn,), daemon=True)
                t.start()
        except OSError:
            pass
        finally:
            try:
                self.sock.close()
            except Exception:
                pass

    def serve(self, conn):
        buf = bytearray()
        try:
            conn.settimeout(30)
            while True:
                chunk = conn.recv(65536)
                if not chunk:
                    return
                with self.lock:
                    self.rawlog += chunk
                buf += chunk
                while True:
                    consumed = self.handle(conn, buf)
                    if consumed == 0:
                        break
                    del buf[:consumed]
        except Exception:
            pass
        finally:
            try:
                conn.close()
            except Exception:
                pass

    def read_line(self, buf):
        i = buf.find(b"\r\n")
        if i < 0:
            return None, 0
        return buf[:i], i + 2

    def parse_cmd(self, buf):
        """Returns (argc argv, consumed) or (None, 0) when incomplete."""
        if not buf:
            return None, 0
        c = buf[0:1]
        if c in (b"*",):
            line, n = self.read_line(buf)
            if line is None:
                return None, 0
            try:
                nargs = int(line[1:])
            except ValueError:
                return ("INLINE", [buf.split(b"\r\n")[0]]), len(buf.split(b"\r\n")[0]) + 2
            pos = n
            args = []
            for _ in range(nargs):
                if pos >= len(buf):
                    return None, 0
                if buf[pos:pos+1] != b"$":
                    # not bulk — try inline remainder
                    return None, 0
                line, n2 = self.read_line(buf[pos:])
                if line is None:
                    return None, 0
                try:
                    blen = int(line[1:])
                except ValueError:
                    return None, 0
                pos += n2
                if pos + blen + 2 > len(buf):
                    return None, 0
                args.append(bytes(buf[pos:pos+blen]))
                pos += blen + 2
            return ("ARRAY", args), pos
        # inline command
        line, n = self.read_line(buf)
        if line is None:
            return None, 0
        return ("INLINE", line.split()), n

    def handle(self, conn, buf):
        parsed, consumed = self.parse_cmd(bytes(buf))
        if parsed is None:
            return 0
        _kind, argv = parsed
        if not argv:
            return consumed
        cmd = argv[0].decode("latin1").upper()
        try:
            if cmd == "PING":
                conn.sendall(b"+PONG\r\n")
            elif cmd == "ECHO":
                arg = argv[1] if len(argv) > 1 else b""
                conn.sendall(b"$%d\r\n" % len(arg) + arg + b"\r\n")
            elif cmd == "SET":
                self.store[argv[1]] = argv[2]
                conn.sendall(b"+OK\r\n")
            elif cmd == "GET":
                v = self.store.get(argv[1])
                if v is None:
                    conn.sendall(b"$-1\r\n")
                else:
                    conn.sendall(b"$%d\r\n" % len(v) + v + b"\r\n")
            elif cmd == "DEL":
                n = 0
                for k in argv[1:]:
                    if self.store.pop(k, None) is not None:
                        n += 1
                conn.sendall(b":%d\r\n" % n)
            elif cmd == "INCR":
                v = int(self.store.get(argv[1], b"0")) + 1
                self.store[argv[1]] = str(v).encode()
                conn.sendall(b":%d\r\n" % v)
            elif cmd == "BIGVAL":
                # 1 MiB deterministic bulk reply
                v = bytes(range(256)) * 4096
                conn.sendall(b"$%d\r\n" % len(v) + v + b"\r\n")
            elif cmd == "BINVAL":
                v = bytes(range(256))  # binary-safe payload incl \r\n
                conn.sendall(b"$%d\r\n" % len(v) + v + b"\r\n")
            elif cmd == "NESTED":
                conn.sendall(b"*2\r\n*2\r\n:1\r\n:2\r\n$3\r\nabc\r\n")
            elif cmd == "NULLARR":
                conn.sendall(b"*-1\r\n")
            elif cmd == "NULLBULK":
                conn.sendall(b"$-1\r\n")
            elif cmd == "EMPTYSTR":
                conn.sendall(b"$0\r\n\r\n")
            elif cmd == "INT":
                conn.sendall(b":12345\r\n")
            elif cmd == "BIGINT":
                conn.sendall(b":9223372036854775807\r\n")
            elif cmd == "MYERR":
                conn.sendall(b"-ERR my deliberate error\r\n")
            elif cmd == "PUSH1":
                conn.sendall(b">3\r\n$7\r\nmessage\r\n$1\r\nc\r\n$1\r\nv\r\n")
                conn.sendall(b"+OK\r\n")
            elif cmd == "HELLO":
                if MODE_FLAGS.get("resp3"):
                    # proper RESP3 map: bulk key "proto" -> integer 3
                    conn.sendall(b"%2\r\n$5\r\nproto\r\n:3\r\n")
                else:
                    # RESP2 downgrade path: refuse HELLO like Redis < 6 does
                    conn.sendall(b"-ERR unknown command 'HELLO'\r\n")
            elif cmd == "RAWLOG":
                with self.lock:
                    raw = bytes(self.rawlog)
                conn.sendall(b"$%d\r\n" % len(raw) + raw + b"\r\n")
            elif cmd == "DIE":
                conn.sendall(b"+BYE\r")
                conn.close()   # premature close mid-reply
                return 0
            elif cmd == "SLOW":
                time.sleep(1.2)
                conn.sendall(b"+slow\r\n")
            else:
                conn.sendall(b"-ERR unknown command '%s'\r\n" % argv[0])
        except Exception:
            return 0
        return consumed

# ---------------------------------------------------------------- TCP mode
class TCP(threading.Thread):
    """Echo server with per-request scenarios. First byte(s) sent by the client
    select the scenario: 'E' echo, 'C' echo-then-close, 'H' echo + FIN
    (half-close), 'R' reset immediately, 'D' dribble 3 chunks then close,
    'Z' zero bytes then close, 'B' echo binary-safe all-256 pattern."""
    def __init__(self, sock):
        super().__init__(daemon=True)
        self.sock = sock

    def run(self):
        try:
            while True:
                conn, _ = self.sock.accept()
                t = threading.Thread(target=self.serve, args=(conn,), daemon=True)
                t.start()
        except OSError:
            pass
        finally:
            try:
                self.sock.close()
            except Exception:
                pass

    def serve(self, conn):
        try:
            conn.settimeout(10)
            first = conn.recv(1)
            if not first:
                return
            mode = first.decode("latin1")
            if mode == "E" or mode == "B":
                conn.sendall(first)
                while True:
                    d = conn.recv(65536)
                    if not d:
                        break
                    conn.sendall(d)
            elif mode == "C":
                conn.sendall(b"bye\n")
                conn.close()
            elif mode == "H":
                d = conn.recv(65536)
                conn.sendall(d)
                conn.shutdown(socket.SHUT_WR)
                time.sleep(0.3)
            elif mode == "R":
                conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                conn.close()
            elif mode == "D":
                for i in range(3):
                    conn.sendall(("part%d" % i).encode())
                    time.sleep(0.1)
                conn.close()
            elif mode == "Z":
                conn.close()
            else:
                conn.sendall(b"?" + first)
        except Exception:
            pass
        finally:
            try:
                conn.close()
            except Exception:
                pass

# ---------------------------------------------------------------- UDP mode
class UDP(threading.Thread):
    def __init__(self, sock):
        super().__init__(daemon=True)
        self.sock = sock

    def run(self):
        try:
            while True:
                d, addr = self.sock.recvfrom(65536)
                if d == b"QUIT":
                    break
                # echo back with a 1-byte prefix marker + sender passthrough
                self.sock.sendto(b">" + d, addr)
        except Exception:
            pass
        finally:
            try:
                self.sock.close()
            except Exception:
                pass

MODES = {"http": HTTP, "resp": RESP, "tcp": TCP, "udp": UDP}
MODE_FLAGS = {}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode")
    ap.add_argument("--portfile", default=None)
    ap.add_argument("--ppid", default=None,
                    help="runner pid; the server exits when orphaned (ppid==1)")
    args = ap.parse_args()
    if args.mode not in MODES:
        die("unknown mode " + args.mode)
    if args.mode == "resp" and os.environ.get("DYN_CTL_RESP3"):
        MODE_FLAGS["resp3"] = 1
    global ONE_MB
    ONE_MB = one_mb_body()
    if args.ppid:
        threading.Thread(target=watch_parent, daemon=True, args=(int(args.ppid),)).start()
    else:
        # stdin-EOF watchdog for interactive use (parent shells)
        threading.Thread(target=watch_stdin, daemon=True).start()
    sock = bind(args.portfile, kind=("udp" if args.mode == "udp" else "tcp"))
    server = MODES[args.mode](sock)
    server.start()
    server.join()

if __name__ == "__main__":
    main()
