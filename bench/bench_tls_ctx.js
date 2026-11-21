import { TCPServer } from "dyna:net";
import { RSA, X509 } from "dyna:crypto";
import { makeTempDir, writeFile, removeAll, Path, readFile } from "dyna:file";

const now = () => performance.now();

function roundTrip(port, tlsOpts, payload) {
    return new Promise((resolve) => {
        const t0 = now();
        let settled = false;
        const done = (v, okv) => { if (!settled) { settled = true; resolve({ v, okv, ms: now() - t0 }); } };
        const to = setTimeout(() => done("TIMEOUT", false), 8000);
        const cli = TCPServer.connect({ host: "127.0.0.1", port, tls: tlsOpts }, {
            connect(c, err) {
                if (err) { clearTimeout(to); done("ERR " + err, false); cli.close(); return; }
                c.write(payload);
            },
            data(c, b) {
                clearTimeout(to);
                const s = new TextDecoder().decode(b);
                done(s, s === payload);
                cli.close();
            },
            close() { clearTimeout(to); done("CLOSED", false); },
        });
    });
}

async function series(srv, opts, n, payload) {
    const out = [];
    for (let i = 0; i < n; i++) {
        const r = await roundTrip(srv.port, opts, payload + i);
        if (!r.okv) throw new Error("series iteration " + i + " failed: " + r.v);
        out.push(r.ms);
    }
    return out;
}

const median = (a) => { const s = [...a].sort((x, y) => x - y); return s[s.length >> 1]; };

async function main() {
    const T = makeTempDir("tlsbench");
    const P = (n) => T + "/" + n;

    const rsa = RSA.generate(2048);
    writeFile(new Path(P("srv.key")), rsa.privateKey);
    writeFile(new Path(P("srv.pem")), X509.generateSelfSigned(
        { key: rsa.privateKey, subject: "localhost", days: 30 }));

    const bundle = readFile(new Path("tests/corpus/ca-bundle.pem"));
    const srvCert = readFile(new Path(P("srv.pem")));
    writeFile(new Path(P("ca-all.pem")), bundle + srvCert);

    const srv = new TCPServer({ port: 0, tls: { cert: P("srv.pem"), key: P("srv.key") } });
    srv.start({ data(c, b) { c.write(b); } });

    print("bench_tls_ctx: server on port " + srv.port +
          ", custom-CA bundle " + (bundle.length / 1024).toFixed(0) + " KiB");

    {
        const opts = { servername: "localhost", ca: P("srv.pem") };
        const t5 = await series(srv, opts, 5, "s");
        print("BENCH tls-session n5=" + t5.map((x) => x.toFixed(2)).join(" ") +
              "ms  (ms per handshake; #1 full, #2..#5 resumed)");
        const t40 = await series(srv, { servername: "localhost",
                                        ca: P("srv.pem"), alpn: ["http/1.1"] },
                                 41, "s");
        const full = t40[0];
        const rest = t40.slice(1);
        print("BENCH tls-session full=" + full.toFixed(2) +
              "ms resumed-avg=" + (rest.reduce((a, b) => a + b, 0) / rest.length).toFixed(2) +
              "ms resumed-median=" + median(rest).toFixed(2) + "ms n=" + rest.length);
    }

    {
        const opts = { servername: "localhost", ca: P("ca-all.pem") };
        const t = await series(srv, opts, 30, "k");
        print("BENCH tls-ctx custom-ca cold=" + t[0].toFixed(2) +
              "ms warm-median=" + median(t.slice(1)).toFixed(2) +
              "ms min=" + Math.min(...t).toFixed(2) + "ms n=" + t.length +
              "  (warm includes session reuse; ctx-only number = pre-session build)");
        const bad = { servername: "localhost", rejectUnauthorized: false };
        const c = await series(srv, bad, 30, "k");
        print("BENCH tls-ctx control(insecure) cold=" + c[0].toFixed(2) +
              "ms warm-median=" + median(c.slice(1)).toFixed(2) + "ms");
    }

    srv.close();
    removeAll(new Path(T));
    print("bench_tls_ctx: done");
}

main().catch((e) => {
    print("bench_tls_ctx: FATAL " + (e && e.message ? e.message : e));
    throw e;
});
