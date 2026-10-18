// tests/lib/sigfuzz_baselines.js -- what sigfuzz must not call, and the valid
// baselines it cannot infer from a signature alone.
//
// Why baselines matter: sigfuzz varies ONE argument at a time and keeps the
// others at their baseline. If any baseline argument is invalid, every
// variant is refused at that argument and the edge values of the others never
// reach the code behind it. A name listed as "never reached" in a run is a
// name whose baseline belongs here.

// SKIP: by bare name (any module) or "module.Label". Network I/O, process
// spawning and filesystem mutation outside the scratch directory are not
// input-handling questions; the suites that own them test them with peers.
export const SKIP = {
    App: 1, WsClient: 1, HTTPServer: 1, HTTPServerAsync: 1, TCPServer: 1, UDPSocket: 1, DNSServer: 1,
    TCPProxy: 1, HTTPClient: 1, Fetcher: 1, fetch: 1, Crawl: 1, DNSResolver: 1, Redis: 1,
    PostgreSQL: 1, SQLite: 1, Watcher: 1, Exec: 1, Spawn: 1, Which: 1, connect: 1, connectHappy: 1,
    listen: 1, serve: 1, resolve: 1, lookup: 1, request: 1,
    removeAll: 1, remove: 1, rename: 1, move: 1, symlink: 1, chmod: 1, chown: 1, chDir: 1,
    setEnv: 1, unsetEnv: 1, exit: 1, kill: 1, sleep: 1, sleepSync: 1, setSigHandler: 1,
    "sys.Spawn#new": 1, "file.makeDir": 1, "file.writeFile": 1, "file.appendFile": 1, "file.copyFile": 1,
    "file.makeTempDir": 1, "file.makeTempFile": 1, "file.truncate": 1, "file.link": 1,
    "compress.ZipExtractAll": "writes a tree; covered by its own suite with a scratch root",
    "bench.run": 1, "bench.compare": 1,
    "scrape.Sitemap.list": "fetches the sitemap over the network; tests/scrape/test_scrape_sitemap.js serves one",
    "sys.setUid": "changes the identity of the process running the suite",
    "sys.setGid": "changes the identity of the process running the suite",
    "uring.open": 1,
    "crypto.RSA.generate": "keygen per call is seconds of CPU; the RSA rows use one cached key",
    "crypto.Argon2id.hash": "memory-hard by design: each edge would allocate its m_cost",
    "crypto.Argon2id.verify": "same cost as hash",
    "crypto.Scrypt": "memory-hard by design",
};

// Names whose work is legitimately linear in a caller array's declared length:
// a sparse array of length 2^32-1 asks them for billions of steps, and they do
// them. Every other name must refuse such an array before working (that is
// what the probe checks), so each entry here says why it is exempt.
// Names with a documented work budget above the default per-call allowance:
// { ms, why }. The allowance is the budget's ceiling, so a call that runs
// past it is still a failure.
const DIFF_BUDGET = { ms: 6000, why: "Myers diff spends up to 2^31 diagonal steps before refusing as too dissimilar (~2 s here)" };
export const SLOW_OK = {
    "matcher.DiffChars": DIFF_BUDGET, "matcher.DiffWords": DIFF_BUDGET, "matcher.DiffLines": DIFF_BUDGET,
};

// Option fields whose value is a resource the caller explicitly asks for: an
// edge value above the cap is skipped, because honouring it is the contract.
// preallocate reserves real disk on Linux (fallocate): the 1e9/2^32 edges
// filled the gate VM's disk and broke unrelated suites.
export const FIELD_MAX = { preallocate: 1 << 20 };

export const HUGE_OK = {
    "random.Random#shuffle": "an in-place Fisher-Yates over the array the caller gave; 16 ns per slot",
    "async.pmap": "one call of fn per index by contract (fn(item, index, items)), at bounded concurrency",
};

import * as os from "os";
import * as std from "std";

const u8 = (...b) => new Uint8Array(b);

// Expensive or stateful fixtures are built once per run and shared.
const memo = new Map();
const memoOnce = (key, make) => { if (!memo.has(key)) memo.set(key, make()); return memo.get(key); };
let uniq = 0;
// A fresh file of its own per call: a File method that removes, moves or
// truncates must not take the fixture away from every method after it.
function freshFile(scratch, text = "line one\nline two\n") {
    const path = `${scratch}/f${++uniq}.txt`;
    const f = std.open(path, "w");
    f.puts(text);
    f.close();
    return path;
}
const cryptoNs = () => memoOnce("crypto-ns", () => import("dyna:crypto"));
const NONCE = new Uint8Array(12).fill(5);
const AEAD_KEY = new Uint8Array(32).fill(3);
const bytesOf = (s) => new TextEncoder().encode(s);
const once = (f) => { let v, done = false; return () => (done ? v : ((done = true), (v = f()))); };

const memSink = () => ({ n: 0, closed: false, write(b) { this.n += b.length; return Promise.resolve(b.length); },
    flush() { return Promise.resolve(); }, close() { this.closed = true; }, [Symbol.dispose]() { this.close(); } });
function smallFrame(ns) {
    return new ns.DataFrame({ x: new Float64Array([1, 2, 3]), y: new Float64Array([3, 2, 1]), w: new Float64Array([1, 1, 1]),
        t: new Float64Array([0, 2, 4]), p: new Int32Array([0, 1, 2]), k: new Int32Array([0, 1, 2]), o: new Int32Array([1, 3, 5]), s: ["a", "b", "c"] });
}

// ---- per-module parameter baselines, by parameter NAME ---------------------
// (pname, kinds, ctx) -> a valid value, or undefined to fall back to the type
export const PARAMS = {
    semver: (p) => ({ rangeString: "^1.2.0", version: "1.2.3", v: "1.2.3", a: "1.2.3", b: "2.0.0-rc.1", range: "^1.0.0", rangeA: "^1.0.0", rangeB: ">=1.2.0 <2.0.0", release: "minor", identifier: "beta", versions: ["1.0.0", "1.2.3", "2.0.0"] })[p],
    uuid: (p) => ({ uuid: "6ba7b810-9dad-11d1-80b4-00c04fd430c8", id: "6ba7b810-9dad-11d1-80b4-00c04fd430c8", a: "6ba7b810-9dad-11d1-80b4-00c04fd430c8", b: "6ba7b811-9dad-11d1-80b4-00c04fd430c8", namespace: "6ba7b810-9dad-11d1-80b4-00c04fd430c8", name: "example.com", bytes: new Uint8Array(16).fill(7), ulid: "01ARZ3NDEKTSV4RRFFQ69G5FAV" })[p],
    time: (p, kinds, ctx) => p === "duration" ? new ctx.ns.Duration({ days: 1, hours: 2 }) : ({ start: "2024-01-01T00:00:00Z", end: "2024-01-10T00:00:00Z", str: "2024-02-29T12:34:56.789Z", text: "2024-02-29 12:34:56", iso: "2024-02-29T12:34:56Z", layout: "2006-01-02", value: "2024-02-29", zone: "UTC", tz: "UTC", year: 2024, month: 2, day: 29, hour: 12, minute: 34, second: 56, rule: "FREQ=DAILY;COUNT=3" })[p],
    // Decimal operands are decimal strings; Money takes integer minor units and an ISO code.
    decimal: (p, kinds) => {
        if (/^(value|x|str)$/.test(p) && kinds.some((k) => k.k === "string")) return p === "str" ? "12.34" : "2.5";
        return { minorUnits: 1234, currency: "USD", n: 3, shares: [1, 2, 1], digits: 2, dp: 2, places: 2 }[p];
    },
    mathx: (p, kinds) => {
        const isArr = kinds.some((k) => k.k === "array" || k.k === "typed" || k.k === "obj");
        if ((p === "a" || p === "b") && kinds.some((k) => k.k === "bigint"))
            return kinds.some((k) => k.k === "number") ? (p === "a" ? 12 : 18) : (p === "a" ? 12n : 18n);
        if ((p === "a" || p === "b") && isArr) return p === "a" ? [1, 2, 4, 8, 3] : [2, 1, 5, 9, 4];
        return { q: 0.25, text: "x * 2 + sin(y)", vars: { x: 3, y: 0.5 } }[p];
    },
    net: (p) => ({ name: "requests_total", value: 3, addr: "192.0.2.10", a: "192.0.2.10", b: "2001:db8::1", prefix: "192.0.2.0/24", cidr: "2001:db8::/32", ip: "192.0.2.10" })[p],
    cli: (p, kinds, ctx) => (p === "sub" ? new ctx.ns.Command("sub") : { flags: "-q, --quiet", args: ["-v", "--name", "x"] }[p]),
    csv: (p) => ({ columns: ["a", "b"], row: 0, value: "v", column: "a", columnIndex: 0, oldName: "a", newName: "z", defaultValue: "d" })[p],
    // index-like parameters of the structures are small valid indices
    structures: (p) => ({ u: 0, v: 1, w: 2, src: 0, dst: 2, x: 0, y: 1, lo: 0, hi: 2 })[p],
    bytes: (p) => ({ label: "utf-8", text: "héllo", offset: 0, value: 7 })[p],
    file: (p, kinds, ctx) => {
        const ns = ctx.ns;
        if (/^dest$/.test(p) && ns && typeof ns.Path === "function") return new ns.Path(`${ctx.scratch}/dest${++uniq}.txt`);
        if (/^(path|dir|target|from|to|src|dst|root)$/i.test(p) && ns && typeof ns.Path === "function")
            return new ns.Path(ctx.scratch + (/dir|root/i.test(p) ? "" : "/seed.txt"));
        return { pattern: "*.txt", glob: "*.txt" }[p];
    },
    encoding: (p) => ({ expression: "$.a[0]", expr: "$.a[0]", path: "$.a", label: "utf-8" })[p],
    crypto: (p) => ({ algorithm: "sha256", alg: "sha256", hash: "sha256", md: "sha256", curve: "P-256" })[p],
    hash: (p) => ({ algorithm: "sha256", alg: "sha256" })[p],
    stream: (p, kinds, ctx) => {
        const ns = ctx.ns;
        if (p === "src") return ns.fromBytes("a\nb\n{\"k\":1}\n");
        if (p === "dst" || p === "sink") return memSink();
        if (p === "opts" && /Inflate|Deflate/.test(JSON.stringify(kinds))) return { codec: "deflate" };
        return undefined;
    },
    json: (p) => ({ pointer: "/a/0", path: "/a/0", doc: { a: [1, 2] }, document: { a: [1, 2] }, value: 3 })[p],
    dataframe: (p, kinds, ctx) => {
        const rows = 8;
        if (p === "other") return smallFrame(ctx.ns);
        const v = {
            time: "t", tau: 2, loCol: "x", hiCol: "y", leftKey: "k", rightKey: "k", leftTime: "o", rightTime: "o",
            index: "k", columns: "s", values: "x", agg: "sum", idVars: ["k"], valueVars: ["x", "y"], how: "inner",
            col: "x", a: "x", b: "y", x: "x", y: "y", key: "k", val: "x", value: "x", valueCol: "x", weightCol: "w",
            timeCol: "t", position: "p", names: ["x", "y"], cols: ["x", "y"], mask: new Uint8Array(rows).fill(1),
            q: 0.5, qs: [0.25, 0.5], k: 3, n: 3, w: 3, bins: 4, buckets: 3, periods: 1, alpha: 0.5, sep: ",",
            lo: -1, hi: 5, fill: 0, start: 0, end: 4, size: 4, every: 2,
        }[p];
        return v;
    },
    ml: (p, kinds, ctx) => {
        const X = [[0, 0], [0, 1], [1, 0], [1, 1], [2, 2], [2, 3], [3, 2], [3, 3]];
        if (p === "path" && ctx.Path) return new ctx.Path(ctx.scratch + "/model.bin");
        if (p === "out") return new Float64Array(64);
        return { X, x: X, y: [0, 0, 0, 0, 1, 1, 1, 1], yTrue: [0, 1, 1, 0], yPred: [0, 1, 0, 0], yScore: [0.1, 0.9, 0.4, 0.2], labels: [0, 1],
            grid: { alpha: [0.1, 1] } }[p];
    },
};

// ---- explicit baseline argument lists: "module.Label" -> (ctx, scratch, ns) => args
const PROTO_SCHEMA = { fields: [{ name: "id", number: 1, type: "int64" }, { name: "name", number: 2, type: "string" }, { name: "tags", number: 3, type: "string", repeated: true }] };
const zipOf = (ns) => ns.ZipPack([{ name: "a.txt", data: bytesOf("A") }, { name: "dir/b.txt", data: bytesOf("bee ".repeat(64)), method: "deflate" }]);
const PKCE_VERIFIER = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";

// Keys are generated once per run; every signature row signs with them.
async function keys() {
    const c = await cryptoNs();
    return memoOnce("keys", () => {
        const ed = c.Ed25519Generate(), x1 = c.X25519Generate(), x2 = c.X25519Generate();
        const rsa = c.RSA.generate(2048), ec1 = c.ECDSA.generate("P-256"), dh1 = c.ECDH.generate("P-256"), dh2 = c.ECDH.generate("P-256");
        const msg = "the message";
        return {
            ed, x1, x2, rsa, ec1, dh1, dh2, msg,
            edSig: c.Ed25519Sign(ed.privateKey, msg),
            rsaSig: c.RSA.sign("sha256", rsa.privateKey, msg),
            pssSig: c.RSA.signPSS("sha256", rsa.privateKey, msg),
            ecSig: c.ECDSA.sign("sha256", ec1.privateKey, msg),
            edPem: c.Ed25519PemFromRaw({ privateKey: ed.privateKey, publicKey: ed.publicKey }).privateKey,
            xPem: c.X25519PemFromRaw({ privateKey: x1.privateKey, publicKey: x1.publicKey }).privateKey,
            cert: c.X509.generateSelfSigned({ key: ec1.privateKey, subject: "sigfuzz", sans: ["example.com", "127.0.0.1"] }),
        };
    });
}
// The AEAD and asymmetric rows, for dyna:crypto (and any module re-exporting them).
function cryptoBaselines(m) {
    const k = (f) => async () => f(await keys(), await cryptoNs());
    const aead = (C) => ({
        [`${m}.${C}#open`]: k((K, c) => [NONCE, new c[C](AEAD_KEY).seal(NONCE, "hello", "aad"), "aad"]),
        [`${m}.${C}#sealInto`]: () => [new Uint8Array(64), NONCE, "hello", "aad"],
        [`${m}.${C}#openInto`]: k((K, c) => [new Uint8Array(64), NONCE, new c[C](AEAD_KEY).seal(NONCE, "hello", "aad"), "aad"]),
    });
    return {
        ...aead("AESGCM"), ...aead("ChaCha20Poly1305"),
        [`${m}.Ed25519Sign`]: k((K) => [K.ed.privateKey, K.msg]),
        [`${m}.Ed25519Verify`]: k((K) => [K.ed.publicKey, K.msg, K.edSig]),
        [`${m}.X25519Derive`]: k((K) => [K.x1.privateKey, K.x2.publicKey]),
        [`${m}.Ed25519PemToRaw`]: k((K) => [K.edPem]),
        [`${m}.Ed25519PemFromRaw`]: k((K) => [{ privateKey: K.ed.privateKey, publicKey: K.ed.publicKey }]),
        [`${m}.X25519PemToRaw`]: k((K) => [K.xPem]),
        [`${m}.X25519PemFromRaw`]: k((K) => [{ privateKey: K.x1.privateKey, publicKey: K.x1.publicKey }]),
        [`${m}.RSA.sign`]: k((K) => ["sha256", K.rsa.privateKey, K.msg]),
        [`${m}.RSA.verify`]: k((K) => ["sha256", K.rsa.publicKey, K.msg, K.rsaSig]),
        [`${m}.RSA.signPSS`]: k((K) => ["sha256", K.rsa.privateKey, K.msg, { saltLen: "auto" }]),
        [`${m}.RSA.verifyPSS`]: k((K) => ["sha256", K.rsa.publicKey, K.msg, K.pssSig]),
        [`${m}.RSA.OAEP.seal`]: k((K) => [K.rsa.publicKey, "hello"]),
        [`${m}.RSA.OAEP.open`]: k((K, c) => [K.rsa.privateKey, c.RSA.OAEP.seal(K.rsa.publicKey, "hello")]),
        [`${m}.X509.parse`]: k((K) => [K.cert]),
        [`${m}.X509.generateSelfSigned`]: k((K) => [{ key: K.ec1.privateKey, subject: "s", days: 30, sans: ["a.example", "::1", "x@example.com"] }]),
        [`${m}.ECDSA.sign`]: k((K) => ["sha256", K.ec1.privateKey, K.msg]),
        [`${m}.ECDSA.verify`]: k((K) => ["sha256", K.ec1.publicKey, K.msg, K.ecSig]),
        [`${m}.ECDH.derive`]: k((K) => [K.dh1.privateKey, K.dh2.publicKey]),
    };
}

// Request/Response/Metrics live in dyna:http and are re-exported by dyna:net.
const MP_BODY = "--XyZ\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\n1\r\n--XyZ\r\nContent-Disposition: form-data; name=\"f\"; filename=\"f.txt\"\r\nContent-Type: text/plain\r\n\r\nhello\r\n--XyZ--\r\n";
function fetchBaselines(m) {
    return {
        [`${m}.Response#instance`]: (c, s, ns) => new ns.Response('{"a":[1,2]}', { status: 200, headers: { "content-type": "application/json" } }),
        [`${m}.Request#instance`]: (c, s, ns) => new ns.Request("http://example.com/p?q=1", { method: "POST", body: '{"a":[1,2]}', headers: { "content-type": "application/json" } }),
        // each metric kind gets its own name: a name is bound to the kind that first used it
        [`${m}.Metrics.gauge`]: () => ["sigfuzz_gauge", 3, { k: "v" }],
        [`${m}.Metrics.histogram`]: () => ["sigfuzz_seconds", 0.2, { k: "v" }],
        [`${m}.MultipartParse`]: () => ["multipart/form-data; boundary=XyZ", bytesOf(MP_BODY)],
    };
}

export const BASELINES = {
    "simd.gemv": () => [new Float32Array(2), new Float32Array(6).fill(1), new Float32Array(3).fill(1), 2, 3, 1, 0],
    "simd.gemvT": () => [new Float32Array(3), new Float32Array(6).fill(1), new Float32Array(2).fill(1), 2, 3, 0],
    "simd.gemm": () => [new Float32Array(4), new Float32Array(6).fill(1), new Float32Array(6).fill(1), 2, 2, 3, 1, 0],
    "uuid.v8": () => [{ fill: new Uint8Array(16).fill(9) }],
    "uuid.v3": () => ["6ba7b810-9dad-11d1-80b4-00c04fd430c8", "example.com"],
    "uuid.v5": () => ["6ba7b810-9dad-11d1-80b4-00c04fd430c8", "example.com"],
    "compress.gunzip": (c, s, ns) => [ns.gzip(bytesOf("hello, hello"))],
    "compress.unzstd": (c, s, ns) => [ns.zstd(bytesOf("hello, hello"))],
    "compress.unbrotli": (c, s, ns) => [ns.brotli(bytesOf("hello, hello"))],
    "compress.lz4Unframe": (c, s, ns) => [ns.lz4Frame(bytesOf("hello, hello"))],
    "compress.inflate": (c, s, ns) => [ns.deflate(bytesOf("hello, hello"))],
    "compress.ZipList": (c, s, ns) => [zipOf(ns)],
    "compress.ZipRead": (c, s, ns) => [zipOf(ns), "a.txt"],
    "compress.ZipReadAt": (c, s, ns) => [zipOf(ns), 1],
    "compress.Dictionary#decompress": (c, s, ns) => [new ns.Dictionary(["hello", "world"]).compress("hello world, hello")],
    "file.readDir": (c, s, ns) => [new ns.Path(s)],
    "file.readLink": (c, s, ns) => {
        const link = `${s}/link${++uniq}`;
        os.symlink("seed.txt", link);
        return [new ns.Path(link)];
    },
    "file.File#instance": (c, s, ns) => new ns.File(new ns.Path(freshFile(s))),
    "file.FileReader#instance": (c, s, ns) => new ns.FileReader(new ns.Path(freshFile(s))),
    "file.FileWriter#instance": (c, s, ns) => new ns.FileWriter(new ns.Path(`${s}/w${++uniq}.txt`)),
    "decimal.Decimal#toBigInt@this": (c, s, ns) => new ns.Decimal("-9007199254740993"),
    "cli.Command#instance": (c, s, ns) => new ns.Command("app").option("-v, --verbose").option("-n, --name <s>"),
    "cli.Spinner#stop@this": (c, s, ns) => new ns.Spinner({ text: "t" }).start(),
    "random.Random#setState": (c, s, ns) => [new ns.Random(7).getState()],
    "serialize.Proto.encode": () => [{ id: 7, name: "n", tags: ["a", "b"] }, PROTO_SCHEMA],
    "serialize.Proto.decode": (c, s, ns) => [ns.Proto.encode({ id: 7, name: "n", tags: ["a", "b"] }, PROTO_SCHEMA), PROTO_SCHEMA],
    "csv.CSVFile#new": (c, s, ns) => [new c.Path(freshFile(s, "a,b\n1,2\n3,4\n"))],
    "net.RateLimiter#new": () => [{ tokensPerSec: 10, burst: 5 }],
    ...fetchBaselines("net"), ...fetchBaselines("http"),
    "mathx.stats.cov": () => [[1, 2, 4, 8, 3], [2, 1, 5, 9, 4]],
    "mathx.stats.corr": () => [[1, 2, 4, 8, 3], [2, 1, 5, 9, 4]],
    "mathx.Expression#eval": () => [{ x: 3, y: 0.5 }],
    "mathx.solveSPD": () => [Float64Array.of(4, 2, 2, 3), Float64Array.of(2, 1)],
    "simd.gather": () => [new Float32Array(3), Float32Array.of(1, 2, 3, 4), Int32Array.of(3, 0, 1)],
    "simd.f64Gather": () => [new Float64Array(3), Float64Array.of(1, 2, 3, 4), Int32Array.of(3, 0, 1)],
    "simd.scatterAdd": () => [new Float32Array(4), Int32Array.of(3, 0, 3), Float32Array.of(1, 2, 3)],
    "simd.f64ScatterAdd": () => [new Float64Array(4), Int32Array.of(3, 0, 3), Float64Array.of(1, 2, 3)],
    "structures.Graph#instance": (c, s, ns) => new ns.Graph({ directed: true, weighted: true }).addEdge(0, 1, 1).addEdge(1, 2, 2).addEdge(0, 2, 5),
    "structures.Graph#mst@this": (c, s, ns) => new ns.Graph({ weighted: true }).addEdge(0, 1, 1).addEdge(1, 2, 2).addEdge(0, 2, 5),
    "structures.UnionFind#new": () => [4],
    "structures.KDTree#new": () => [[[0, 0], [1, 2], [3, 1], [2, 2]]],
    "structures.KDTree#box": () => [[0, 0], [2, 2]],
    "structures.IndexedHeap#pushMany": () => [[1, 2, 3], [0.5, 2, 1]],
    "compress.Compressor#decompress": async (c) => [(await c.fresh("Compressor")).compress("hello, hello, hello")],
    "csv.CSVFile#updateCell": () => [{ row: 0, column: "a", value: "v" }],
    "csv.CSVFile#create@this": (c, s, ns) => new ns.CSVFile(new c.Path(`${s}/new${++uniq}.csv`)),
    "structures.CountMinSketch#merge": async (c) => [await c.fresh("CountMinSketch")],
    "time.Duration#new": () => [{ days: 1, hours: 2 }],
    "time.RRule#new": () => [{ freq: "DAILY", count: 3, dtstart: "2024-01-01T00:00:00Z" }],
    "ml.Pipeline#new": (c, s, ns) => [[new ns.StandardScaler(), new ns.LogisticRegression()]],
    "time.parseDate": () => ["2024-02-29"],
    "time.parseTime": () => ["12:34:56.789"],
    "time.Format#parse": () => ["2024-02-29"],
    "xml.XmlWriter#instance": (c, s, ns) => { const w = new ns.XmlWriter(); w.open("a"); w.open("b"); return w; },
    "xml.SAXParser#instance": (c, s, ns) => { const p = new ns.SAXParser(); p.write("<a k='v'>t<b/><!-- c --></a>"); return p; },
    "url.URL#new": () => ["https://user@example.com:8080/a/b?x=1#f", "https://example.org/"],
    "url.URL.join": () => ["https://example.com/a/b", "../c?d=1"],
    "oauth2.generateCodeChallenge": () => [PKCE_VERIFIER, "S256"],
    "oauth2.verifyCodeChallenge": (c, s, ns) => [PKCE_VERIFIER, ns.generateCodeChallenge(PKCE_VERIFIER, "S256"), "S256"],
    "oauth2.verifyJWT": async () => [(await cryptoNs()).JWTSign({ sub: "a", aud: "app", iss: "me", scope: "read write", exp: 4102444800 }, "k", { alg: "HS256" }), "k",
        { algorithms: ["HS256"], aud: "app", iss: "me", requiredScope: ["read"] }],
    "oauth2.buildAuthorizationUrl": (c, s, ns) => [{ authorizationEndpoint: "https://auth.example.com/authorize", clientId: "client", redirectUri: "https://app.example.com/cb",
        scope: "read", state: "st", codeChallenge: ns.generateCodeChallenge(PKCE_VERIFIER, "S256"), codeChallengeMethod: "S256" }],
    "oauth2.parseTokenResponse": () => ['{"access_token":"tok","token_type":"Bearer","expires_in":3600,"refresh_token":"r","scope":"read"}'],
    ...cryptoBaselines("crypto"),
    "encoding.Base58CheckDecode": (c, s, ns) => [ns.Base58CheckEncode(u8(1, 2, 3))],
    "encoding.base58CheckDecodeInto": (c, s, ns) => [ns.Base58CheckEncode(u8(1, 2, 3)), new Uint8Array(64)],
    "crypto.HMAC": () => ["sha256", "key", "data"],
    "crypto.PBKDF2": () => [{ hash: "sha256", password: "pw", salt: "salt0000", iterations: 2, length: 16 }],
    "crypto.JWTVerify": (c, s, ns) => [ns.JWTSign({ sub: "a" }, "k", { alg: "HS256" }), "k", { algorithms: ["HS256"] }],
    "async.withTimeout": () => [Promise.resolve(1), 50],
    "decimal.Money.fromDecimal": (c, s, ns) => [new ns.Decimal("12.34"), "USD"],
    "time.RRule.fromString": () => ["FREQ=DAILY;COUNT=3"],
    "bytes.Bytes#new": () => [new Uint8Array(64).fill(1)],
    "time.PlainDate#new": () => [2024, 2, 29],
    "compress.Dictionary#new": () => [["hello", "world"]],
    "crypto.AESGCM#new": () => [new Uint8Array(32).fill(3)],
    "crypto.ChaCha20Poly1305#new": () => [new Uint8Array(32).fill(3)],
    "crypto.Hmac#new": () => ["sha256", "key"],
    ...Object.fromEntries(["SUM_CHECKED", "BITWISE_AND", "BITWISE_OR", "BITWISE_XOR", "GROUP_BITMAP"].map((v) => [`dataframe.DataFrame#${v}`, () => ["k"]])),
    ...Object.fromEntries(["GROUP_BIT_AND", "GROUP_BIT_OR", "GROUP_BIT_XOR"].map((v) => [`dataframe.DataFrame#${v}`, () => ["k", "p"]])),
    ...Object.fromEntries(["JSON_AGG_STRICT", "JSON_OBJECT_AGG_STRICT"].map((v) => [`dataframe.DataFrame#${v}`, () => ["k", "y"]])),
    "dataframe.DataFrame#DROP_NA": () => ["x", "y"],
    // a right-side string column is refused (no value for an unmatched row)
    "dataframe.DataFrame#ASOF_JOIN": (c, s, ns) => [new ns.DataFrame({ v: new Float64Array([9, 8, 7]), o: new Int32Array([1, 3, 5]) }), "o", "o"],
    "ml.CSR#new": () => [[1, 2, 3], [0, 1, 0], [0, 2, 3], 2],
    "ml.crossValScore": (c, s, ns) => [() => new ns.LinearRegression(), [[0], [1], [2], [3], [4], [5]], [0, 2, 4, 6, 8, 10], { k: 2 }],
    "ml.gridSearch": (c, s, ns) => [(p) => new ns.LogisticRegression(p), [[0], [1], [2], [3], [4], [5]], [0, 0, 0, 1, 1, 1], { C: [0.1, 1] }, { k: 2 }],
    "ml.randomSearch": (c, s, ns) => [(p) => new ns.LogisticRegression(p), [[0], [1], [2], [3], [4], [5]], [0, 0, 0, 1, 1, 1], { C: [0.1, 1] }, { k: 2, nIter: 2 }],
    "dataframe.DataFrame#instance": (c, s, ns) => new ns.DataFrame({
        x: new Float64Array([1, 2, NaN, 4, 5, -6, 7, 8]),
        y: new Float64Array([2, 4, 6, 8, 10, 12, 14, 16]),
        w: new Float64Array([1, 1, 2, 2, 1, 0, 1, 1]),
        t: new Float64Array([0, 1, 2, 3, 4, 5, 6, 7]),
        p: new Int32Array([0, 1, 2, 3, 0, 1, 2, 3]),
        k: new Int32Array([0, 1, 0, 2, 1, 0, 2, 1]),
        o: new Int32Array([0, 1, 2, 3, 4, 5, 6, 7]),
        s: ["a", "b", "a", "c", "b", "a", "c", "b"],
    }),
};

// Generic hooks, consulted when no "module.Class#instance" / "module.Label"
// entry exists. dyna:ml: every class with a fit() is fitted on a small
// separable problem before its methods run (an unfitted model refuses
// predict/transform at the boundary and proves nothing about them), and its
// static deserialize/load get bytes and a file that model produced.
const MLX = [[0, 0], [0, 1], [1, 0], [1, 1], [2, 2], [2, 3], [3, 2], [3, 3]];
const MLY = [0, 0, 0, 0, 1, 1, 1, 1];
export const INSTANCE = {
    ml: async (name, ctx, ns) => {
        const C = ns[name];
        if (typeof C !== "function" || !C.prototype || typeof C.prototype.fit !== "function") return undefined;
        const given = BASELINES["ml." + name + "#new"];
        const m = new C(...(given ? given(ctx, ctx.scratch, ns) : []));
        try { await m.fit(MLX, MLY); } catch (e) { try { await m.fit(MLX); } catch (e2) {} }
        return m;
    },
};
// A receiver in a particular state, by module: (cname, method) -> a receiver
// factory, or undefined for the shared instance. dyna:structures serializes a
// fresh instance: the shared one holds whatever hostile values earlier calls
// stored in it, which serialize rightly refuses.
export const THIS = {
    // parse runs on the baseline Command: earlier argument()/option() edge cases
    // register required arguments that parse then rightly demands
    cli: (cname, mname) => (cname === "Command" && mname === "parse" ? (c) => c.fresh(cname) : undefined),
    // a body is read once: every reader gets a Request/Response of its own
    net: (cname, mname) => (/^(Request|Response)$/.test(cname) && /^(json|text|bytes|arrayBuffer|blob|formData|clone)$/.test(mname) ? (c) => c.fresh(cname) : undefined),
    http: (cname, mname) => (/^(Request|Response)$/.test(cname) && /^(json|text|bytes|arrayBuffer|blob|formData|clone)$/.test(mname) ? (c) => c.fresh(cname) : undefined),
    // and a Graph query runs on the baseline graph: addEdge's edge cases leave
    // negative and non-finite weights the shortest-path algorithms refuse
    structures: (cname, mname) => (mname === "serialize" || (cname === "Graph" && !/^(add|remove)/.test(mname)) ? (c) => c.fresh(cname) : undefined),
};
export const FIXED = {
    structures: async (label, ctx) => {
        const m = /^(\w+)\.deserialize$/.exec(label);
        if (!m) return undefined;
        const inst = await ctx.fresh(m[1]);
        return inst ? [inst.serialize()] : undefined;
    },
    ml: async (label, ctx, scratch, ns) => {
        const m = /^(\w+)\.(deserialize|load)$/.exec(label);
        if (!m) return undefined;
        const inst = await ctx.prepare(m[1]);
        if (!inst) return undefined;
        if (m[2] === "deserialize") return [inst.serialize()];
        if (!ctx.Path) return undefined;
        const p = new ctx.Path(scratch + "/" + m[1] + ".model");
        await inst.save(p);
        return [p];
    },
};
