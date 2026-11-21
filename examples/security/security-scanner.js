// Security scanner — secrets, code injection patterns, infrastructure misconfiguration and vulnerable dependencies
// in one pass over a repository, with stable fingerprints, a baseline, SARIF output and a CI gate.
//
// WHAT IT IS
//   The scanner a CI pipeline runs on every pull request:
//
//   discovery    walks the tree honouring .scannerignore globs, skips binaries by content sniffing, and uses a
//                multi-pattern prefilter so files with no trigger keyword never reach the expensive rules
//   secrets      provider-specific patterns (AWS, GitHub, Slack, Stripe, private keys, JWTs) plus a high-entropy
//                rule for generic assignments; JWTs are decoded to report their algorithm and expiry; test
//                fixtures and documented placeholders are recognised and downgraded
//   code         injection sinks reached by request data within a function: SQL built by concatenation or
//                template strings, shell commands, eval/new Function, path traversal, open redirects, weak hashes
//   infra        Dockerfiles (latest tags, root user, curl | sh, ADD from URLs), Kubernetes YAML (privileged,
//                hostNetwork, no limits, secrets in env), IAM JSON (Action/Resource "*")
//   dependencies package.json + lockfile versions matched against advisories with semver ranges
//   results      each finding gets a fingerprint from rule + file + normalised snippet (stable when lines move);
//                a baseline suppresses accepted findings; output is SARIF 2.1.0, an HTML report and a table;
//                the gate fails only on NEW findings at or above the threshold severity
//   reuse        every matched secret value is hashed (never stored) into a Multimap of value -> files; a credential
//                that appears in two or more files is one leak in several places and gets its own finding
//   triage       a Multiset counts findings per rule for the summary line
//
// MODULES  dyna:file (readDir, sniffType, glob) · dyna:matcher (MultiMatcher) · dyna:yaml · dyna:semver
//          · dyna:hash (XXH3) · dyna:encoding (Base64URL) · dyna:html (Template) · dyna:cli (Table)
//          · dyna:async (pmap) · dyna:structures (Multimap, Multiset)
//
// RUN      dynajs examples/security/security-scanner.js [dir]     scan a directory (default: a generated sample repo)

import { Path, makeTempDir, makeDir, writeFile, readFile, readBytes, readDir, sniffType, removeAll } from "dyna:file";
import { MultiMatcher } from "dyna:matcher";
import * as YAML from "dyna:yaml";
import * as semver from "dyna:semver";
import { XXH3_64 } from "dyna:hash";
import { Base64URLDecode } from "dyna:encoding";
import { Template } from "dyna:html";
import { Table } from "dyna:cli";
import { pmap } from "dyna:async";
import { Multimap, Multiset } from "dyna:structures";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const PLANTED_STRIPE_KEY = ["sk", "live", "4eC39HqLyjWDarjtT1zdp7dc"].join("_");
const selfTest = scriptArgs.length < 2;
const work = makeTempDir("scan");
const root = selfTest ? writeSampleRepo(work.join("repo")) : new Path(scriptArgs[1]);
const SEV = { critical: 4, high: 3, medium: 2, low: 1, note: 0 };

// =============================================================================
// Discovery
// =============================================================================
const globRe = (g) =>
    new RegExp(
        "^" +
            g
                .split("**")
                .map((p) => p.replace(/[.+^${}()|[\]\\]/g, "\\$&").replace(/\*/g, "[^/]*"))
                .join(".*") +
            "$",
    );
let ignore = [];
try {
    ignore = readFile(root.join(".scannerignore"))
        .split("\n")
        .map((l) => l.trim())
        .filter((l) => l && !l.startsWith("#"))
        .map(globRe);
} catch {}
const files = [];
let skippedBinary = 0,
    skippedIgnored = 0;
(function walk(dir, rel) {
    for (const e of readDir(dir).sort((a, b) => a.name.localeCompare(b.name))) {
        const r = rel ? `${rel}/${e.name}` : e.name;
        if (e.name === ".git" || e.name === "node_modules") continue;
        if (ignore.some((re) => re.test(r))) {
            skippedIgnored++;
            continue;
        }
        if (e.isDir) walk(dir.join(e.name), r);
        else if (e.isFile) files.push(r);
    }
})(root, "");

// =============================================================================
// Rules
// =============================================================================
const SECRET_RULES = [
    { id: "SEC001", name: "AWS access key id", sev: "critical", re: /\b(AKIA|ASIA)[0-9A-Z]{16}\b/g },
    { id: "SEC002", name: "GitHub token", sev: "critical", re: /\bgh[pousr]_[A-Za-z0-9]{36}\b/g },
    { id: "SEC003", name: "Slack token", sev: "high", re: /\bxox[baprs]-[0-9A-Za-z-]{10,}\b/g },
    { id: "SEC004", name: "Stripe live key", sev: "critical", re: /\bsk_live_[0-9a-zA-Z]{24,}\b/g },
    { id: "SEC005", name: "Private key", sev: "critical", re: /-----BEGIN (RSA |EC |OPENSSH |)PRIVATE KEY-----/g },
    {
        id: "SEC006",
        name: "JSON Web Token",
        sev: "medium",
        re: /\beyJ[A-Za-z0-9_-]{8,}\.eyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b/g,
    },
];
const CODE_RULES = [
    {
        id: "INJ001",
        name: "SQL built from request data",
        sev: "high",
        sink: /\.(query|exec|raw)\s*\(\s*(`[^`]*\$\{|["'][^"']*["']\s*\+)/,
        tainted: true,
    },
    {
        id: "INJ002",
        name: "Shell command from request data",
        sev: "critical",
        sink: /\b(exec|execSync|spawn|system|popen)\s*\(/,
        tainted: true,
    },
    {
        id: "INJ003",
        name: "Dynamic code evaluation",
        sev: "high",
        sink: /\beval\s*\(|new Function\s*\(/,
        tainted: false,
    },
    {
        id: "INJ004",
        name: "Path traversal",
        sev: "high",
        sink: /\b(readFile|open|sendFile|createReadStream)\s*\([^)]*(req\.|params|query)/,
        tainted: true,
    },
    {
        id: "INJ005",
        name: "Open redirect",
        sev: "medium",
        sink: /\bredirect\s*\(\s*req\.(query|params|body)/,
        tainted: false,
    },
    {
        id: "CRY001",
        name: "Weak hash for security purpose",
        sev: "medium",
        sink: /\b(md5|sha1|MD5|SHA1)\s*\(.*(password|token|secret)/i,
        tainted: false,
    },
];
const SOURCES = /\breq\.(query|params|body|headers|cookies)|\brequest\.(args|form|json)|\bprocess\.argv/;
const TRIGGERS = new MultiMatcher([
    "AKIA",
    "ASIA",
    "ghp_",
    "gho_",
    "ghs_",
    "xox",
    "sk_live_",
    "PRIVATE KEY",
    "eyJ",
    "query",
    "exec",
    "spawn",
    "system",
    "eval",
    "Function",
    "readFile",
    "sendFile",
    "redirect",
    "md5",
    "sha1",
    "MD5",
    "SHA1",
    "secret",
    "token",
    "password",
    "key",
    "apikey",
    "FROM",
    "privileged",
    "Action",
    "dependencies",
]);
const PLACEHOLDER = /(example|dummy|placeholder|changeme|xxxx|your[_-]?key|<[^>]+>)/i;

const entropy = (s) => {
    const f = new Map();
    for (const c of s) f.set(c, (f.get(c) || 0) + 1);
    let h = 0;
    for (const n of f.values()) {
        const p = n / s.length;
        h -= p * Math.log2(p);
    }
    return h;
};
const findings = [];
const secretSites = new Multimap(); // XXH3(secret value) -> "file:line"
const noisiest = new Multiset();
function add(rule, file, line, snippet, extra = {}) {
    const norm = snippet.trim().replace(/\s+/g, " ").replace(/\d+/g, "0");
    noisiest.add(rule.id);
    findings.push({
        rule: rule.id,
        name: rule.name,
        sev: extra.sev || rule.sev,
        file,
        line,
        snippet: snippet.trim().slice(0, 140),
        fingerprint: XXH3_64(`${rule.id}|${file}|${norm}`, 0, { as: "hex" }),
        ...extra,
    });
}
const isTestPath = (f) => /(^|\/)(test|tests|__tests__|fixtures|spec)\//.test(f) || /\.(test|spec)\.\w+$/.test(f);
function scanSecrets(file, lines) {
    lines.forEach((text, i) => {
        for (const r of SECRET_RULES)
            for (const m of text.matchAll(r.re)) {
                const extra = {};
                if (r.id === "SEC006") {
                    try {
                        const head = JSON.parse(new TextDecoder().decode(Base64URLDecode(m[0].split(".")[0]))),
                            body = JSON.parse(new TextDecoder().decode(Base64URLDecode(m[0].split(".")[1])));
                        extra.detail = `alg=${head.alg}${body.exp ? `, exp=${new Date(body.exp * 1000).toISOString().slice(0, 10)}` : ", no expiry"}`;
                        if (head.alg === "none") extra.sev = "high";
                    } catch {
                        continue;
                    }
                }
                if (PLACEHOLDER.test(text) || isTestPath(file)) extra.sev = "low";
                else secretSites.put(XXH3_64(m[0], 0, { as: "hex" }), `${file}:${i + 1}`);
                add(r, file, i + 1, text.replace(m[0], m[0].slice(0, 6) + "…redacted"), extra);
            }
        const g = /\b([A-Za-z_]*(secret|token|passw(or)?d|api_?key)[A-Za-z_]*)\s*[:=]\s*["']([^"'\s]{16,})["']/i.exec(
            text,
        );
        if (
            g &&
            entropy(g[4]) > 3.8 &&
            !SECRET_RULES.some((r) => {
                r.re.lastIndex = 0;
                return r.re.test(g[4]);
            })
        ) {
            add(
                { id: "SEC010", name: "High-entropy secret assignment", sev: "high" },
                file,
                i + 1,
                text.replace(g[4], g[4].slice(0, 4) + "…redacted"),
                {
                    detail: `entropy ${entropy(g[4]).toFixed(2)} bits/char`,
                    ...(PLACEHOLDER.test(text) || isTestPath(file) ? { sev: "low" } : {}),
                },
            );
        }
    });
}
function scanCode(file, lines) {
    let fnStart = 0,
        taintedVars = new Set();
    lines.forEach((text, i) => {
        if (/\bfunction\b|=>\s*\{|^\s*def\s/.test(text)) {
            fnStart = i;
            taintedVars = new Set();
        }
        const assign =
            /\b(?:const|let|var)\s+(?:\{\s*([^}]+)\}|(\w+))\s*=\s*(.+)/.exec(text) || /^\s*(\w+)\s*=\s*(.+)/.exec(text);
        if (SOURCES.test(text)) {
            if (assign)
                (assign[1] || assign[2] || "")
                    .split(",")
                    .map((v) => v.trim().split(":").pop().trim())
                    .filter(Boolean)
                    .forEach((v) => taintedVars.add(v));
        } else if (assign && [...taintedVars].some((v) => new RegExp(`\\b${v}\\b`).test(assign[3] || assign[2] || "")))
            taintedVars.add(assign[2] || assign[1]);
        for (const r of CODE_RULES) {
            if (!r.sink.test(text)) continue;
            const usesTaint = SOURCES.test(text) || [...taintedVars].some((v) => new RegExp(`\\b${v}\\b`).test(text));
            if (r.tainted && !usesTaint) continue;
            add(r, file, i + 1, text, {
                detail: usesTaint ? `request data reaches the sink (function at line ${fnStart + 1})` : undefined,
            });
        }
    });
}
function scanDockerfile(file, lines) {
    let user = "root";
    lines.forEach((text, i) => {
        const t = text.trim();
        if (/^FROM\s+[^\s:@]+(:latest)?(\s|$)/i.test(t) && !/@sha256:/.test(t))
            add({ id: "IAC001", name: "Base image not pinned", sev: "medium" }, file, i + 1, t);
        if (/^USER\s+(\S+)/i.test(t)) user = /^USER\s+(\S+)/i.exec(t)[1];
        if (/(curl|wget)[^|]*\|\s*(ba)?sh/.test(t))
            add({ id: "IAC002", name: "Remote script piped to a shell", sev: "high" }, file, i + 1, t);
        if (/^ADD\s+https?:/i.test(t))
            add({ id: "IAC003", name: "ADD from a URL (no checksum)", sev: "medium" }, file, i + 1, t);
        if (/^(ENV|ARG)\s+\w*(SECRET|TOKEN|PASSWORD|KEY)\w*[= ]/i.test(t))
            add({ id: "IAC004", name: "Secret baked into image layer", sev: "high" }, file, i + 1, t);
    });
    if (user === "root" || user === "0")
        add({ id: "IAC005", name: "Container runs as root", sev: "medium" }, file, lines.length, "(no non-root USER)");
}
function scanKubernetes(file, text) {
    let docs;
    try {
        docs = YAML.ParseAll(text);
    } catch (e) {
        add({ id: "IAC090", name: "Unparseable YAML", sev: "note" }, file, 1, e.message);
        return;
    }
    const lineOf = (needle) => Math.max(1, text.split("\n").findIndex((l) => l.includes(needle)) + 1);
    for (const d of docs) {
        const spec = d?.spec?.template?.spec || d?.spec;
        if (!spec?.containers) continue;
        if (spec.hostNetwork)
            add(
                { id: "K8S001", name: "hostNetwork enabled", sev: "high" },
                file,
                lineOf("hostNetwork"),
                `${d.kind}/${d.metadata?.name} hostNetwork: true`,
            );
        for (const c of spec.containers) {
            const where = `${d.kind}/${d.metadata?.name} container ${c.name}`;
            if (c.securityContext?.privileged)
                add({ id: "K8S002", name: "Privileged container", sev: "critical" }, file, lineOf("privileged"), where);
            if (c.securityContext?.allowPrivilegeEscalation !== false)
                add(
                    { id: "K8S003", name: "Privilege escalation not disabled", sev: "low" },
                    file,
                    lineOf(`name: ${c.name}`),
                    where,
                );
            if (!c.resources?.limits)
                add(
                    { id: "K8S004", name: "No resource limits", sev: "medium" },
                    file,
                    lineOf(`name: ${c.name}`),
                    where,
                );
            for (const e of c.env || [])
                if (/secret|token|password|key/i.test(e.name) && e.value)
                    add(
                        { id: "K8S005", name: "Secret value in env (use secretKeyRef)", sev: "high" },
                        file,
                        lineOf(e.name),
                        `${where} env ${e.name}`,
                    );
        }
    }
}
function scanIam(file, text) {
    let doc;
    try {
        doc = JSON.parse(text);
    } catch {
        return;
    }
    if (!doc.Statement) return;
    for (const st of [].concat(doc.Statement)) {
        const acts = [].concat(st.Action || []),
            res = [].concat(st.Resource || []);
        if (st.Effect === "Allow" && acts.includes("*") && res.includes("*"))
            add({ id: "IAM001", name: "Allow *:* policy", sev: "critical" }, file, 1, JSON.stringify(st).slice(0, 120));
        else if (st.Effect === "Allow" && acts.some((a) => /:\*$/.test(a)) && res.includes("*"))
            add(
                { id: "IAM002", name: "Service wildcard on all resources", sev: "high" },
                file,
                1,
                JSON.stringify(st).slice(0, 120),
            );
    }
}
const ADVISORIES = [
    { id: "CVE-2021-23337", package: "lodash", range: "<4.17.21", sev: "high", title: "command injection in template" },
    {
        id: "CVE-2022-24999",
        package: "qs",
        range: ">=6.7.0 <6.7.3 || >=6.5.0 <6.5.3",
        sev: "high",
        title: "prototype pollution",
    },
    { id: "CVE-2024-29041", package: "express", range: "<4.19.2", sev: "medium", title: "open redirect" },
    {
        id: "CVE-2020-7660",
        package: "serialize-javascript",
        range: "<3.1.0",
        sev: "high",
        title: "remote code execution",
    },
];
function scanDependencies(file, text) {
    let pkg;
    try {
        pkg = JSON.parse(text);
    } catch {
        return;
    }
    const deps = { ...pkg.dependencies, ...pkg.devDependencies };
    for (const [name, spec] of Object.entries(deps)) {
        const version = semver.coerce(spec);
        if (!version) continue;
        for (const a of ADVISORIES)
            if (a.package === name && semver.satisfies(version, a.range))
                add(
                    { id: "DEP001", name: `${a.id}: ${a.title}`, sev: a.sev },
                    file,
                    Math.max(1, text.split("\n").findIndex((l) => l.includes(`"${name}"`)) + 1),
                    `${name}@${version}`,
                    { detail: `fixed in ${a.range.replace("<", ">=")}`, dev: !!pkg.devDependencies?.[name] },
                );
    }
}

// =============================================================================
// Scan
// =============================================================================
const started = performance.now();
let prefiltered = 0;
await pmap(
    files,
    async (file) => {
        const p = root.join(file);
        const kind = sniffType(p);
        if (!/^text\/|json|xml|yaml|javascript|x-sh|^application\/octet-stream$/.test(kind) && kind !== "") {
            skippedBinary++;
            return;
        }
        const bytes = readBytes(p);
        if (bytes.subarray(0, 1024).includes(0)) {
            skippedBinary++;
            return;
        }
        const text = new TextDecoder().decode(bytes);
        if (!TRIGGERS.test(text) && !TRIGGERS.test(text.toLowerCase())) {
            prefiltered++;
            return;
        }
        const lines = text.split("\n");
        scanSecrets(file, lines);
        if (/\.(js|mjs|ts|py)$/.test(file)) scanCode(file, lines);
        if (/(^|\/)Dockerfile[^/]*$/.test(file)) scanDockerfile(file, lines);
        if (/\.ya?ml$/.test(file)) scanKubernetes(file, text);
        if (/iam.*\.json$|policy.*\.json$/.test(file)) scanIam(file, text);
        if (/(^|\/)package\.json$/.test(file)) scanDependencies(file, text);
    },
    { concurrency: 8 },
);
for (const [, sites] of secretSites.keys().map((k) => [k, secretSites.get(k)])) {
    if (sites.length < 2) continue;
    const [file, line] = sites[0].split(":");
    add(
        { id: "SEC011", name: "Same credential committed in several files", sev: "critical" },
        file,
        Number(line),
        `reused in ${sites.join(", ")}`,
        { detail: `${sites.length} copies` },
    );
}
const elapsed = performance.now() - started;
findings.sort((a, b) => SEV[b.sev] - SEV[a.sev] || a.file.localeCompare(b.file) || a.line - b.line);

// =============================================================================
// Baseline, gate, outputs
// =============================================================================
let baseline = new Set();
try {
    baseline = new Set(JSON.parse(readFile(root.join(".scanner-baseline.json"))).fingerprints);
} catch {}
const fresh = findings.filter((f) => !baseline.has(f.fingerprint));
const GATE = "high";
const blocking = fresh.filter((f) => SEV[f.sev] >= SEV[GATE]);
const sarif = {
    version: "2.1.0",
    $schema: "https://json.schemastore.org/sarif-2.1.0.json",
    runs: [
        {
            tool: {
                driver: {
                    name: "dyna-scan",
                    version: "1.0.0",
                    rules: [
                        ...new Map(
                            findings.map((f) => [f.rule, { id: f.rule, shortDescription: { text: f.name } }]),
                        ).values(),
                    ],
                },
            },
            results: findings.map((f) => ({
                ruleId: f.rule,
                level: SEV[f.sev] >= 3 ? "error" : SEV[f.sev] === 2 ? "warning" : "note",
                message: { text: `${f.name}${f.detail ? ` (${f.detail})` : ""}` },
                locations: [{ physicalLocation: { artifactLocation: { uri: f.file }, region: { startLine: f.line } } }],
                partialFingerprints: { primaryLocationLineHash: f.fingerprint },
                baselineState: baseline.has(f.fingerprint) ? "unchanged" : "new",
            })),
        },
    ],
};
writeFile(work.join("results.sarif"), JSON.stringify(sarif, null, 1));
const REPORT =
    new Template(`<!doctype html><meta charset="utf-8"><title>Security scan</title><h1>{{total}} findings, {{blocking}} blocking</h1>
<table><tr><th>Severity</th><th>Rule</th><th>Location</th><th>Detail</th></tr>{{#rows}}<tr class="{{sev}}"><td>{{sev}}</td><td>{{rule}} {{name}}</td><td>{{file}}:{{line}}</td><td>{{snippet}}</td></tr>{{/rows}}</table>`);
writeFile(
    work.join("report.html"),
    REPORT.render({ total: findings.length, blocking: blocking.length, rows: findings }),
);

print("security-scanner");
print(
    `  scanned   ${files.length} files in ${elapsed.toFixed(0)} ms (${prefiltered} skipped by keyword prefilter, ${skippedBinary} binary, ${skippedIgnored} ignored)`,
);
print(
    table([
        ["sev", "rule", "location", "finding"],
        ...findings
            .slice(0, 40)
            .map((f) => [
                f.sev,
                f.rule,
                `${f.file}:${f.line}`,
                `${f.name}${f.detail ? ` — ${f.detail}` : ""}`.slice(0, 70),
            ]),
    ]),
);
print(
    `  noisiest  ${noisiest
        .entrySet()
        .sortBy(([, n]) => -n)
        .take(5)
        .map(([r, n]) => `${r} ${n}`)
        .join(", ")}`,
);
print(
    `  gate      ${blocking.length} new finding(s) at >= ${GATE}; ${findings.length - fresh.length} suppressed by baseline -> ${blocking.length ? "FAIL" : "PASS"}`,
);

if (selfTest) {
    const results = {};
    const has = (rule, file) => findings.some((f) => f.rule === rule && f.file.endsWith(file));
    results["provider secrets detected"] =
        has("SEC001", "src/config.js") && has("SEC004", "src/billing.js") && has("SEC005", "deploy/id_rsa.pem");
    results["a key in a test fixture is downgraded"] =
        findings.find((f) => f.rule === "SEC001" && f.file.startsWith("test/"))?.sev === "low";
    results["high-entropy assignment found; placeholder ignored"] =
        has("SEC010", "src/settings.py") && !findings.some((f) => f.rule === "SEC010" && /changeme/.test(f.snippet));
    results["an alg=none JWT is decoded and escalated"] = findings.some(
        (f) => f.rule === "SEC006" && /alg=none/.test(f.detail) && f.sev === "high",
    );
    results["SQL concatenation flagged, parameterised query not"] =
        has("INJ001", "src/routes/users.js") &&
        !findings.some((f) => f.rule === "INJ001" && f.snippet.includes("WHERE id = ?"));
    results["shell, traversal and redirect sinks found"] =
        has("INJ002", "src/routes/admin.js") &&
        has("INJ004", "src/routes/files.js") &&
        has("INJ005", "src/routes/auth.js");
    results["Dockerfile misconfigurations"] =
        has("IAC001", "Dockerfile") &&
        has("IAC002", "Dockerfile") &&
        has("IAC004", "Dockerfile") &&
        has("IAC005", "Dockerfile");
    results["Kubernetes misconfigurations"] =
        has("K8S002", "k8s/deploy.yaml") && has("K8S005", "k8s/deploy.yaml") && has("K8S001", "k8s/deploy.yaml");
    results["IAM wildcard policy"] = has("IAM001", "infra/iam-admin.json") && !has("IAM001", "infra/iam-readonly.json");
    results["vulnerable dependencies by semver range"] =
        findings
            .filter((f) => f.rule === "DEP001")
            .map((f) => f.snippet)
            .sort()
            .join() === "express@4.17.1,lodash@4.17.15,qs@6.7.0";
    results["a credential committed in two files is reported once as reused"] =
        findings.filter((f) => f.rule === "SEC011").length === 1 &&
        /src\/billing\.js.*deploy\/\.env|deploy\/\.env.*src\/billing\.js/.test(
            findings.find((f) => f.rule === "SEC011").snippet,
        );
    results["ignored paths and binary files are skipped"] =
        !files.some((f) => f.startsWith("vendor/")) && skippedBinary >= 1;
    results["secrets are redacted in the output"] = !JSON.stringify(findings).includes(PLANTED_STRIPE_KEY);
    // Line shift: insert lines at the top of a file, rescan -> identical fingerprints.
    const fp1 = findings
        .filter((f) => f.file === "src/routes/users.js")
        .map((f) => f.fingerprint)
        .sort()
        .join();
    const src = readFile(root.join("src/routes/users.js"));
    writeFile(root.join("src/routes/users.js"), "// header\n// more header\n" + src);
    const before = findings.length;
    findings.length = 0;
    scanCode("src/routes/users.js", readFile(root.join("src/routes/users.js")).split("\n"));
    results["fingerprints survive a line shift"] =
        findings
            .map((f) => f.fingerprint)
            .sort()
            .join() === fp1 && findings[0].line > 2;
    results["the baseline suppresses an accepted finding and the gate fails on new ones"] =
        blocking.length > 0 && blocking.every((f) => !baseline.has(f.fingerprint)) && before - fresh.length === 1;
    results["SARIF written"] =
        sarif.runs[0].results.length === before && JSON.parse(readFile(work.join("results.sarif"))).version === "2.1.0";
    removeAll(work);
    check(results);
}

// ---- a deliberately flawed sample repository --------------------------------------
function writeSampleRepo(dir) {
    const w = (rel, text) => {
        const p = dir.join(rel);
        makeDir(p.dirname, { recursive: true });
        writeFile(p, text);
    };
    const jwtNone =
        "eyJhbGciOiJub25lIiwidHlwIjoiSldUIn0.eyJzdWIiOiJhZG1pbiIsInJvbGUiOiJhZG1pbiJ9.c2lnbmF0dXJlLXBsYWNlaG9sZGVy";
    w(
        "src/config.js",
        `export const aws = { accessKeyId: "AKIAZ7VQ3XK2LMN8RT4P", region: "eu-west-1" };\nexport const session = "${jwtNone}";\n`,
    );
    w(
        "src/billing.js",
        `const stripe = require("stripe")("${PLANTED_STRIPE_KEY}");\nexport const md5Password = (password) => md5(password + salt);\n`,
    );
    w(
        "src/settings.py",
        `DEBUG = False\nSECRET_KEY = "q8Zt!fK2#vL9wPx4mN7rB1cY6hJ3dS0g"\nAPI_TOKEN = "changeme-changeme-changeme"\n`,
    );
    w(
        "src/routes/users.js",
        `export function getUser(req, res) {
    const id = req.params.id;
    const rows = db.query(\`SELECT * FROM users WHERE id = \${id}\`);
    res.json(rows);
}
export function safeUser(req, res) {
    const id = req.params.id;
    const rows = db.query("SELECT * FROM users WHERE id = ?", [id]);
    res.json(rows);
}
export function search(req, res) {
    const { term } = req.query;
    const like = "%" + term + "%";
    return db.query("SELECT * FROM items WHERE name LIKE '" + like + "'");
}
`,
    );
    w(
        "src/routes/admin.js",
        `import { exec } from "child_process";\nexport function ping(req, res) {\n    const host = req.query.host;\n    exec("ping -c 1 " + host, (e, out) => res.send(out));\n}\nexport const calc = (expr) => eval(expr);\n`,
    );
    w(
        "src/routes/files.js",
        `export function download(req, res) {\n    res.sendFile("/srv/files/" + req.params.name);\n}\n`,
    );
    w("src/routes/auth.js", `export function callback(req, res) {\n    res.redirect(req.query.next);\n}\n`);
    w("src/util.js", `export const add = (a, b) => a + b;\nexport const VERSION = "1.0.0";\n`);
    w("deploy/.env", `STRIPE_KEY=${PLANTED_STRIPE_KEY}\n`);
    w("test/fixtures/creds.js", `export const fake = "AKIAIOSFODNN7EXAMPLE";\n`);
    w(
        "deploy/id_rsa.pem",
        "-----BEGIN RSA PRIVATE KEY-----\nMIIEowIBAAKCAQEA0Z3VS5JJcds3xfn/ygWyF8PbnGy0AHB7MaUSs\n-----END RSA PRIVATE KEY-----\n",
    );
    w(
        "Dockerfile",
        `FROM node:latest\nARG NPM_TOKEN=npm_abcdef\nRUN curl -fsSL https://get.example.sh | bash\nADD https://example.com/tool.tar.gz /opt/\nCOPY . /app\nCMD ["node", "/app/server.js"]\n`,
    );
    w(
        "k8s/deploy.yaml",
        `apiVersion: apps/v1
kind: Deployment
metadata: { name: api }
spec:
  template:
    spec:
      hostNetwork: true
      containers:
        - name: api
          image: registry.example/api:1.2.3
          securityContext: { privileged: true }
          env:
            - name: DB_PASSWORD
              value: hunter2hunter2
        - name: sidecar
          image: registry.example/proxy:2.0.0
          securityContext: { allowPrivilegeEscalation: false }
          resources: { limits: { cpu: 200m, memory: 128Mi } }
---
apiVersion: v1
kind: Service
metadata: { name: api }
spec: { ports: [{ port: 80 }] }
`,
    );
    w(
        "infra/iam-admin.json",
        JSON.stringify({ Version: "2012-10-17", Statement: [{ Effect: "Allow", Action: "*", Resource: "*" }] }),
    );
    w(
        "infra/iam-readonly.json",
        JSON.stringify({
            Version: "2012-10-17",
            Statement: [{ Effect: "Allow", Action: ["s3:GetObject"], Resource: "arn:aws:s3:::bucket/*" }],
        }),
    );
    w(
        "package.json",
        JSON.stringify(
            {
                name: "app",
                dependencies: { express: "^4.17.1", lodash: "4.17.15", qs: "6.7.0", "left-pad": "1.3.0" },
                devDependencies: { "serialize-javascript": "^3.1.0" },
            },
            null,
            2,
        ),
    );
    w("vendor/lib/huge.js", `const key = "AKIAZ7VQ3XK2LMN8RT4Q";\n`);
    w(".scannerignore", "# third-party code\nvendor/**\n");
    makeDir(dir.join("assets"), { recursive: true });
    writeFile(
        dir.join("assets/logo.png"),
        new Uint8Array([
            0x89,
            0x50,
            0x4e,
            0x47,
            0x0d,
            0x0a,
            0x1a,
            0x0a,
            0,
            0,
            0,
            13,
            73,
            72,
            68,
            82,
            ...new Array(64).fill(0),
        ]),
    );
    w("README", "Sample service. Set your API key in the environment.\n");
    const accepted = XXH3_64(`IAC005|Dockerfile|(no non-root USER)`, 0, { as: "hex" });
    w(
        ".scanner-baseline.json",
        JSON.stringify({ fingerprints: [accepted], note: "root user accepted until the base image migration" }),
    );
    return dir;
}

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
