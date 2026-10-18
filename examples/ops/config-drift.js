// Config drift — a fleet's YAML configs canonicalised and fingerprinted, drift found by grouping fingerprints, the
// exact changed paths reported, and remediation rolled out in dependency order and in waves.
//
// WHAT IT IS
//   What a configuration-management run (Puppet/Ansible/Chef "drift report") does across 60 hosts:
//
//   canonical   each host's YAML is parsed (dyna:yaml) and re-serialised with sorted keys (StableStringify), so
//               comments, key order and indentation never count as drift; the canonical text is SHA-256 fingerprinted
//   grouping    hosts grouped by fingerprint in a Multimap; the majority fingerprint per role is the inferred golden
//               config and must equal the declared desired state
//   diffs       a structural diff lists changed paths (services.nginx.workers: 4 -> 16); DiffLines on the canonical
//               JSON, pretty-printed, shows the same change as a line diff for the ticket
//   remediate   services restart in dependency order (Graph topologicalSort: db before api before web); hosts are
//               fixed in waves — 1 canary, then 25%, then the rest — never more than maxUnavailable at once
//   verify      after remediation every host's fingerprint equals its role's golden fingerprint
//
// MODULES  dyna:yaml (Parse, Stringify) · dyna:encoding (StableStringify) · dyna:hash (SHA256Hex) · dyna:matcher
//          (DiffLines) · dyna:structures (Multimap, Graph, Multiset) · dyna:random · dyna:cli
//
// RUN      dynajs examples/ops/config-drift.js

import { Parse as parseYaml, Stringify as toYaml } from "dyna:yaml";
import { StableStringify } from "dyna:encoding";
import { SHA256Hex } from "dyna:hash";
import { DiffLines } from "dyna:matcher";
import { Multimap, Graph, Multiset } from "dyna:structures";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(2005);

// =============================================================================
// Desired state per role
// =============================================================================
const DESIRED = {
    web: {
        services: {
            nginx: { workers: 4, keepalive: 65, tls: { protocols: ["TLSv1.2", "TLSv1.3"], hsts: true } },
            node_exporter: { port: 9100 },
        },
        sysctl: { "net.core.somaxconn": 4096 },
        users: ["deploy", "ops"],
    },
    api: {
        services: {
            app: { replicas: 3, heap_mb: 2048, env: { LOG_LEVEL: "info", FEATURE_X: "off" } },
            node_exporter: { port: 9100 },
        },
        sysctl: { "vm.swappiness": 10 },
        users: ["deploy", "ops"],
    },
    db: {
        services: {
            postgres: { max_connections: 300, shared_buffers: "8GB", wal_level: "replica" },
            node_exporter: { port: 9100 },
        },
        sysctl: { "vm.swappiness": 1 },
        users: ["dba", "ops"],
    },
};
const DEPENDS = { web: ["api"], api: ["db"], db: [] };

// Render a host's YAML with cosmetic noise: shuffled key order, comments, varying indentation.
function render(obj, indent) {
    const shuffle = (o) =>
        Array.isArray(o)
            ? o.map(shuffle)
            : o && typeof o === "object"
              ? Object.fromEntries(
                    Object.entries(o)
                        .sort(() => rng.nextFloat() - 0.5)
                        .map(([k, v]) => [k, shuffle(v)]),
                )
              : o;
    return `# managed by cfg-agent, rendered ${rng.nextBounded(1e6)}\n` + toYaml(shuffle(obj), { indent });
}
const clone = (o) => JSON.parse(JSON.stringify(o));
const hosts = [];
const planted = new Map();
for (let i = 0; i < 60; i++) {
    const role = ["web", "api", "db"][i % 3];
    const actual = clone(DESIRED[role]);
    let drift = null;
    if (i % 7 === 4) {
        actual.services.nginx
            ? ((actual.services.nginx.workers = 16), (drift = ["services.nginx.workers"]))
            : actual.services.app
              ? ((actual.services.app.env.LOG_LEVEL = "debug"), (drift = ["services.app.env.LOG_LEVEL"]))
              : ((actual.services.postgres.max_connections = 1000), (drift = ["services.postgres.max_connections"]));
    }
    if (i === 13) {
        actual.users.push("contractor");
        drift = ["users[2]"];
    }
    if (i === 29) {
        delete actual.services.node_exporter;
        drift = ["services.node_exporter"];
    }
    if (drift) planted.set(`host-${i}`, drift);
    hosts.push({ name: `host-${i}`, role, yaml: render(actual, [2, 4][i % 2]) });
}

// =============================================================================
// Canonicalise, fingerprint, group
// =============================================================================
const fingerprint = (obj) => SHA256Hex(StableStringify(obj));
for (const h of hosts) {
    h.parsed = parseYaml(h.yaml);
    h.fp = fingerprint(h.parsed);
}
const byRole = new Map();
for (const h of hosts) {
    const g = byRole.get(h.role) ?? byRole.set(h.role, new Multimap()).get(h.role);
    g.put(h.fp, h.name);
}
const golden = {};
for (const [role, groups] of byRole)
    golden[role] = groups
        .keys()
        .map((fp) => [fp, groups.count(fp)])
        .sort((a, b) => b[1] - a[1])[0][0];
results["cosmetic differences (order, comments, indentation) do not change fingerprints"] = hosts
    .filter((h) => !planted.has(h.name))
    .every((h) => h.fp === golden[h.role]);
results["the majority fingerprint per role equals the declared desired state"] = Object.keys(DESIRED).every(
    (r) => golden[r] === fingerprint(DESIRED[r]),
);
const drifted = hosts.filter((h) => h.fp !== golden[h.role]);
results["exactly the planted hosts are drifted"] =
    JSON.stringify(drifted.map((h) => h.name).sort()) === JSON.stringify([...planted.keys()].sort());

// =============================================================================
// Structural diff and line diff
// =============================================================================
function diffPaths(want, have, path = "") {
    if (Array.isArray(want) || Array.isArray(have)) {
        if (!Array.isArray(want) || !Array.isArray(have)) return [path];
        const out = [];
        for (let i = 0; i < Math.max(want.length, have.length); i++)
            out.push(...diffPaths(want[i], have[i], `${path}[${i}]`));
        return out;
    }
    if (want && typeof want === "object" && have && typeof have === "object") {
        const out = [];
        for (const k of new Set([...Object.keys(want), ...Object.keys(have)]))
            out.push(...diffPaths(want[k], have[k], path ? `${path}.${k}` : k));
        return out;
    }
    return want === have ? [] : [path];
}
const report = [];
let pathsExact = true,
    lineDiffsOk = true;
for (const h of drifted) {
    const paths = diffPaths(DESIRED[h.role], h.parsed);
    if (JSON.stringify(paths) !== JSON.stringify(planted.get(h.name))) pathsExact = false;
    // RFC 8785 canonical JSON is always compact; re-parse it (keys stay sorted) and pretty-print for a readable line diff.
    const pretty = (o) => JSON.stringify(JSON.parse(StableStringify(o)), null, 2);
    const lines = DiffLines(pretty(DESIRED[h.role]), pretty(h.parsed)).filter((x) => x.op !== 0);
    if (!lines.length) lineDiffsOk = false;
    report.push([
        h.name,
        h.role,
        paths.join(", "),
        lines
            .map((l) => `${l.op > 0 ? "+" : "-"}${l.text.trim()}`)
            .join(" ")
            .truncate(50),
    ]);
}
print(Table(report, { head: ["host", "role", "changed paths", "line diff"] }));
results["structural diff names exactly the planted paths"] = pathsExact;
results["every drifted host has a non-empty line diff"] = lineDiffsOk;

// =============================================================================
// Remediation: dependency order, then waves under maxUnavailable
// =============================================================================
const roles = Object.keys(DEPENDS),
    g = new Graph({ directed: true });
roles.forEach(() => g.addNode());
for (const [role, deps] of Object.entries(DEPENDS))
    for (const d of deps) g.addEdge(roles.indexOf(d), roles.indexOf(role));
const roleOrder = g.topologicalSort().map((i) => roles[i]);
results["dependencies order remediation db -> api -> web"] = roleOrder.join(",") === "db,api,web";
const MAX_UNAVAILABLE = 3,
    waves = [];
for (const role of roleOrder) {
    const queue = drifted.filter((h) => h.role === role);
    if (!queue.length) continue;
    const sizes = [1, Math.max(1, Math.ceil(queue.length * 0.25))];
    let i = 0,
        w = 0;
    while (i < queue.length) {
        const size = Math.min(MAX_UNAVAILABLE, sizes[w] ?? queue.length - i);
        waves.push({ role, hosts: queue.slice(i, i + size) });
        i += size;
        w++;
    }
}
const touched = new Multiset();
for (const wave of waves)
    for (const h of wave.hosts) {
        h.parsed = clone(DESIRED[h.role]);
        h.yaml = render(h.parsed, 2);
        h.fp = fingerprint(parseYaml(h.yaml));
        touched.add(h.name);
    }
results["every wave respects maxUnavailable"] = waves.every((w) => w.hosts.length <= MAX_UNAVAILABLE);
results["each role starts with a single canary"] = roleOrder
    .filter((r) => waves.some((w) => w.role === r))
    .every((r) => waves.find((w) => w.role === r).hosts.length === 1);
results["each drifted host is remediated exactly once"] =
    drifted.every((h) => touched.count(h.name) === 1) && touched.elementSet().length === drifted.length;
results["after remediation every host matches its golden fingerprint"] = hosts.every((h) => h.fp === golden[h.role]);
print(
    `remediation: ${waves.length} waves (${waves.map((w) => `${w.role}:${w.hosts.length}`).join(" ")}), fleet of ${hosts.length} converged on ${new Set(hosts.map((h) => h.fp)).size} fingerprints`,
);

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
