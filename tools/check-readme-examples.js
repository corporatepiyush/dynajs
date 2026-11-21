// flags: --std
// Every fenced code block in a markdown file is checked; none is silently skipped.
//
//   ./dynajs --std tools/check-readme-examples.js [FILE.md] [path/to/dynajs]
//   ./dynajs --std tools/check-readme-examples.js selftest
//
// A README example that does not run is worse than no example: it is a confident-looking lie.
// What each fence gets:
//   ```js     executed with the binary under a wall-clock and a CPU-time bound (TIMEOUT is told
//             apart from FAIL). A block that needs the network or never returns (a server that runs
//             until stopped) carries, on the line above its fence,
//                 <!-- check:offline: REASON -->
//             and runs with a preamble instead of being skipped: dyna:http App.start() returns at
//             once, dyna:scrape Fetcher.get() is answered by the real Fetcher against a loopback
//             HTTPServer holding canned pages (so the response has the real shape), and fetch()
//             throws. Constructors, option validation and everything after the network call run.
//   ```json   parsed.
//   ```text   a block starting "usage: dynajs" must equal `dynajs --help` (minus its version line).
//   ```sh     every `--flag` given to dynajs, install.sh or build.sh must appear in that
//             program's own help, and the block carries <!-- check:norun: REASON --> saying why it
//             is not executed (it installs, builds, or names a file the reader supplies).
// CHECK_SHOW=1 prints the output of the blocks that pass too.
// A REASON has at least 12 characters. The old bare <!-- check:skip --> is refused: a block that
// is never run is exactly where wrong option names hid (README recipe 5 used Fetcher options
// paceMs/jitter, and res.ok/res.text that a FetcherResponse does not have).
import * as std from "std";
import * as os from "os";
import { Exec } from "dyna:sys";

let WALL_MS = Number(std.getenv("CHECK_WALL_LIMIT") || 20) * 1000;
let CPU_S = Number(std.getenv("CHECK_CPU_LIMIT") || 20);
const TMP = (std.getenv("TMPDIR") || "/tmp").replace(/\/$/, "") + "/readme-examples." + os.getpid();

export function blocks(md) {
    const lines = md.split("\n"), out = [], errors = [];
    for (let i = 0; i < lines.length; i++) {
        const m = /^```(\S*)\s*$/.exec(lines[i]);
        if (!m) continue;
        const start = i, lang = m[1];
        let j = i + 1;
        while (j < lines.length && lines[j] !== "```") j++;
        if (j >= lines.length) { errors.push(`line ${start + 1}: unterminated fence`); break; }
        const prev = start > 0 ? lines[start - 1] : "";
        const mk = /^<!--\s*check:(\w+)(?::\s*(.*?))?\s*-->$/.exec(prev);
        out.push({ line: start + 1, lang, body: lines.slice(start + 1, j).join("\n") + "\n", mark: mk ? mk[1] : null, reason: mk ? (mk[2] || "") : "" });
        i = j;
    }
    return { list: out, errors };
}

export const OFFLINE_PREAMBLE = `import * as __net from "dyna:net";
import * as httpMod from "dyna:http";
import * as __scrape from "dyna:scrape";
const __page = (t) => "<html><head><title>" + t + "</title></head><body><h1>" + t + "</h1><a href=\\"/docs/a\\">a</a> <a href=\\"https://elsewhere.example/x\\">x</a> <a href=\\"b\\">b</a></body></html>";
const __srv = new httpMod.HTTPServer({ port: 0, workers: 1, routes: {
    "/robots.txt": { status: 200, contentType: "text/plain", body: "User-agent: *\\nAllow: /\\n" },
    "/docs": { status: 200, contentType: "text/html", body: __page("Docs") },
    "/blog": { status: 200, contentType: "text/html", body: __page("Blog") },
} });
__srv.start();
const __local = new __scrape.Fetcher({ agent: "readme-check/1.0", robots: true, minDelayMs: 0, allowPrivateHosts: true });
const __get = __scrape.Fetcher.prototype.get;
__scrape.Fetcher.prototype.get = function (url) {
    if (this === __local) return __get.call(this, url);
    const r = __get.call(__local, "http://127.0.0.1:" + __srv.port + new URL(url).pathname);
    r.url = url;
    return r;
};
httpMod.App.prototype.start = function () { return this; };
globalThis.fetch = async () => { throw new Error("check-readme-examples: an offline block called fetch()"); };
`;
export const OFFLINE_EPILOGUE = "\n__srv.close();\n__local.close?.();\n";

function sh(cmd) { return Exec("sh", ["-c", cmd], { maxBuffer: 64 << 20 }); }
const q = (s) => "'" + s.replace(/'/g, "'\\''") + "'";
function write(path, text) { const f = std.open(path, "w"); f.puts(text); f.close(); }

function runJs(bin, path) {
    const r = Exec("sh", ["-c", `ulimit -t ${CPU_S} 2>/dev/null; exec ${q(bin)} ${q(path)}`], { timeoutMs: WALL_MS, maxBuffer: 64 << 20 });
    return { ok: !r.timedOut && r.code === 0, timedOut: r.timedOut || r.signal === "SIGXCPU", out: (String(r.stdout) + String(r.stderr)).trim() };
}

function helpOf(bin, prog) {
    if (prog === "dynajs") return String(Exec(bin, ["--help"]).stdout);
    const dir = bin.includes("/") ? bin.slice(0, bin.lastIndexOf("/")) : ".";
    if (prog === "install.sh") return std.loadFile(dir + "/install.sh");
    if (prog === "build.sh") return std.loadFile(dir + "/build.sh");
    return null;
}

export function shFlags(body) {
    const out = [];
    for (const raw of body.split("\n")) {
        const line = raw.replace(/\s+#.*$/, "");
        const cmd = /(?:^|\s|\/)(dynajs|install\.sh|build\.sh)(?=\s|$)/.exec(line);
        if (!cmd) continue;
        const rest = line.slice(cmd.index + cmd[0].length).replace(/'[^']*'|"[^"]*"/g, " ");
        for (const f of rest.matchAll(/(?:^|\s)(--?[A-Za-z][\w-]*)/g)) out.push({ prog: cmd[1], flag: f[1] });
        for (const w of rest.split(/\s+/)) if (cmd[1] === "build.sh" && /^[a-z]+$/.test(w)) out.push({ prog: "build.sh", flag: w });
    }
    return out;
}

export function check(md, bin, say = print) {
    const { list, errors } = blocks(md);
    const bad = [...errors];
    const counts = { js: 0, offline: 0, json: 0, text: 0, sh: 0 };
    Exec("mkdir", ["-p", TMP]);
    const helps = new Map();
    const help = (p) => { if (!helps.has(p)) helps.set(p, helpOf(bin, p)); return helps.get(p); };
    for (const b of list) {
        const where = `line ${b.line} (${b.lang || "no language"})`;
        if (b.mark === "skip") { bad.push(`${where}: <!-- check:skip --> is refused: use check:offline: REASON (js) or check:norun: REASON`); continue; }
        if (b.mark && !["offline", "norun"].includes(b.mark)) { bad.push(`${where}: unknown marker check:${b.mark}`); continue; }
        if (b.mark && b.reason.length < 12) { bad.push(`${where}: check:${b.mark} needs a reason of 12+ characters`); continue; }
        if (b.lang === "js") {
            if (b.mark === "norun") { bad.push(`${where}: a js block runs (check:offline: REASON when it needs the network)`); continue; }
            const off = b.mark === "offline";
            const path = `${TMP}/ex${b.line}.js`;
            write(path, off ? OFFLINE_PREAMBLE + b.body + OFFLINE_EPILOGUE : b.body);
            const r = runJs(bin, path);
            counts[off ? "offline" : "js"]++;
            say(`${r.ok ? "ok  " : r.timedOut ? "TIMEOUT" : "FAIL"} ${where}${off ? " offline" : ""}`);
            if (r.ok && std.getenv("CHECK_SHOW")) say(r.out.split("\n").map((l) => "       " + l).join("\n"));
            if (!r.ok) {
                bad.push(`${where}: ${r.timedOut ? "timed out" : "failed"}`);
                say(r.out.split("\n").slice(-6).map((l) => "       " + l).join("\n"));
            }
        } else if (b.lang === "json") {
            counts.json++;
            try { JSON.parse(b.body); say(`ok   ${where}`); } catch (e) { bad.push(`${where}: not JSON (${e.message})`); }
        } else if (b.lang === "text" && b.body.startsWith("usage: dynajs")) {
            counts.text++;
            const want = help("dynajs").split("\n").filter((l) => !/^DynaJS version /.test(l)).join("\n").trimEnd();
            if (b.body.trimEnd() === want) say(`ok   ${where} matches dynajs --help`);
            else {
                const a = b.body.trimEnd().split("\n"), w = want.split("\n");
                let k = 0;
                while (k < a.length && a[k] === w[k]) k++;
                bad.push(`${where}: differs from dynajs --help at its line ${k + 1}: README ${JSON.stringify(a[k])}, binary ${JSON.stringify(w[k])}`);
            }
        } else if (b.lang === "sh") {
            counts.sh++;
            if (b.mark !== "norun") { bad.push(`${where}: a sh block needs <!-- check:norun: REASON --> (shell blocks are option-checked, not run)`); continue; }
            let n = 0;
            for (const { prog, flag } of shFlags(b.body)) {
                const h = help(prog);
                if (h === null) continue;
                n++;
                const re = new RegExp("(^|[^\\w-])" + flag.replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + "(?![\\w-])");
                if (!re.test(h)) bad.push(`${where}: ${prog} has no option ${flag}`);
            }
            say(`ok   ${where} norun, ${n} option(s) checked`);
        } else {
            if (b.mark !== "norun") bad.push(`${where}: a ${b.lang || "plain"} fence is neither run nor checked: give it a language this tool checks or <!-- check:norun: REASON -->`);
        }
    }
    sh("rm -rf " + q(TMP));
    return { bad, counts, total: list.length };
}

function selftest(bin) {
    const fence = (lang, body, mark) => (mark ? mark + "\n" : "") + "```" + lang + "\n" + body + "\n```\n";
    const good = [
        fence("js", 'print("hi");'),
        fence("js", 'import { Fetcher } from "dyna:scrape";\nconst f = new Fetcher({ agent: "a/1", robots: true, minDelayMs: 5 });\nconst r = await f.get("https://example.com/docs");\nif (r.status !== 200 || !r.body.includes("<h1>Docs</h1>")) throw new Error("canned page " + r.status);', "<!-- check:offline: needs the network -->"),
        fence("js", 'import { App } from "dyna:http";\nconst app = new App({ port: 8080 });\napp.get("/x", () => 1);\napp.start();', "<!-- check:offline: runs until stopped -->"),
        fence("json", '{ "a": [1, 2] }'),
        fence("sh", "dynajs --timeout-ms 5 x.js   # bounded\n./build.sh release", "<!-- check:norun: needs a file the reader supplies -->"),
        fence("text", String(Exec(bin, ["--help"]).stdout).split("\n").filter((l) => !/^DynaJS version /.test(l)).join("\n").trimEnd()),
    ].join("\n");
    const plants = [
        ["a failing js block", fence("js", 'throw new Error("boom");'), /\(js\): failed/],
        ["a bare check:skip", fence("js", 'print(1);', "<!-- check:skip -->"), /check:skip --> is refused/],
        ["a marker without a reason", fence("js", 'print(1);', "<!-- check:offline: net -->"), /needs a reason/],
        ["a wrong Fetcher option in an offline block", fence("js", 'import { Fetcher } from "dyna:scrape";\nnew Fetcher({ agent: "a", paceMs: 1500 });', "<!-- check:offline: needs the network -->"), /\(js\): failed/],
        ["a FetcherResponse field that does not exist", fence("js", 'import { Fetcher } from "dyna:scrape";\nconst r = new Fetcher({ agent: "a" }).get("https://example.com/docs");\nif (typeof r.text !== "string") throw new Error("FetcherResponse has no text: " + Object.keys(r));', "<!-- check:offline: needs the network -->"), /\(js\): failed/],
        ["bad JSON", fence("json", "{ a: 1 }"), /not JSON/],
        ["an unmarked sh block", fence("sh", "dynajs x.js"), /needs <!-- check:norun/],
        ["an unknown dynajs flag", fence("sh", "dynajs --timeout 5 x.js", "<!-- check:norun: needs a file the reader supplies -->"), /dynajs has no option --timeout/],
        ["an unknown install.sh flag", fence("sh", "install.sh --prefixx /opt", "<!-- check:norun: installs software on the host -->"), /install\.sh has no option --prefixx/],
        ["a stale help text", fence("text", "usage: dynajs [options] [file [args]]\n-h  --help   list nothing"), /differs from dynajs --help/],
        ["an unchecked fence", fence("", "plain words"), /neither run nor checked/],
        ["a hanging block", fence("js", "for (;;) {}"), /timed out/],
    ];
    let fails = 0;
    const quiet = () => {};
    const g = check(good, bin, quiet);
    if (g.bad.length) { fails++; print("selftest: the good file was refused:\n  " + g.bad.join("\n  ")); }
    else print(`selftest: ok   good file (${g.counts.js} js, ${g.counts.offline} offline, ${g.counts.json} json, ${g.counts.sh} sh, ${g.counts.text} text)`);
    for (const [what, md, re] of plants) {
        const r = check(md, bin, quiet);
        const hit = r.bad.some((b) => re.test(b));
        if (!hit) fails++;
        print(`selftest: ${hit ? "ok  " : "MISS"} ${what}${hit ? "" : " -- got: " + JSON.stringify(r.bad)}`);
    }
    print(`check-readme-examples selftest: ${fails ? fails + " problem(s)" : "every plant refused, the good file passed"}`);
    return fails === 0;
}

const a1 = scriptArgs[1];
if (a1 === "selftest") {
    CPU_S = 2;
    std.exit(selftest(scriptArgs[2] || "./dynajs") ? 0 : 1);
}
const md = a1 || "README.md", bin = scriptArgs[2] || "./dynajs";
const text = std.loadFile(md);
if (text === null) { print("no such file: " + md); std.exit(2); }
if (os.stat(bin)[1] !== 0) { print("not found: " + bin); std.exit(2); }
const t0 = Date.now();
const r = check(text, bin);
for (const b of r.bad) print("  " + b);
print(`\n${md}: ${r.total} blocks: ${r.counts.js} js run, ${r.counts.offline} js run offline, ${r.counts.json} json parsed, ${r.counts.text} help text compared, ${r.counts.sh} sh option-checked; ${r.bad.length} problem(s) in ${((Date.now() - t0) / 1000).toFixed(1)}s`);
std.exit(r.bad.length ? 1 : 0);
