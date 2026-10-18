// Feature flags — a flag service with segment targeting, sticky percentage rollouts, weighted variants, prerequisite
// flags and ETag-cached config delivery, with the rollout statistics checked by a chi-square test.
//
// WHAT IT IS
//   LaunchDarkly / Unleash in one file:
//
//   config      flags in YAML (dyna:yaml): rules match user attributes (country, plan, email domain, app version via
//               dyna:semver), then a percentage rollout or weighted variants; a kill switch overrides everything
//   bucketing   bucket = XXHash32(flag salt + user id) mod 10 000: deterministic per user, independent across flags,
//               and monotone — raising a rollout from 20% to 50% only ever ADDS users
//   prereqs     a flag may require another flag's variant; the prerequisite graph is topologically sorted with
//               dyna:structures Graph and a cycle is refused at load
//   delivery    GET /flags returns the config with an ETag (SHA-256 of the canonical JSON); a client sending
//               If-None-Match gets 304; GET /evaluate evaluates every flag for one user
//   statistics  50 000 users: per-flag rollout shares, variant weights, a chi-square test that buckets are uniform
//               (p-value from mathx.gammainc), and the correlation between two flags' assignments
//
// MODULES  dyna:yaml (Parse) · dyna:hash (XXHash32, SHA256Hex) · dyna:semver · dyna:structures (Graph, Multiset) ·
//          dyna:mathx (gammainc) · dyna:http (App, fetch) · dyna:encoding (StableStringify) · dyna:cli
//
// RUN      dynajs examples/web/feature-flags.js

import { Parse as parseYaml } from "dyna:yaml";
import { XXHash32, SHA256Hex } from "dyna:hash";
import * as semver from "dyna:semver";
import { Graph, Multiset } from "dyna:structures";
import { gammainc } from "dyna:mathx";
import { App } from "dyna:http";
import { StableStringify } from "dyna:encoding";
import { Table } from "dyna:cli";

const results = {};
const CONFIG_V1 = `
flags:
  new-checkout:
    salt: nc-2026
    rules:
      - when: { email_domain: example.com }
        serve: on
      - when: { country: [DE, AT, CH] }
        rollout: 50
    rollout: 20
  search-v2:
    salt: s2-2026
    variants: { control: 50, bm25: 30, vector: 20 }
  dark-mode:
    salt: dm
    rules:
      - when: { app_version: ">=3.2.0" }
        rollout: 100
    rollout: 0
  vector-reranker:
    salt: vr
    requires: { flag: search-v2, variant: vector }
    rollout: 60
  legacy-export:
    salt: le
    killed: true
    rollout: 100
`;

// =============================================================================
// Loading and validation
// =============================================================================
function load(text) {
    const cfg = parseYaml(text);
    const names = Object.keys(cfg.flags),
        g = new Graph({ directed: true });
    const id = new Map(names.map((n) => [n, (g.addNode(), names.indexOf(n))]));
    for (const [name, f] of Object.entries(cfg.flags)) {
        if (f.requires) {
            if (!id.has(f.requires.flag)) throw new Error(`${name}: unknown prerequisite ${f.requires.flag}`);
            g.addEdge(id.get(f.requires.flag), id.get(name));
        }
        if (f.variants && Object.values(f.variants).sum() !== 100)
            throw new Error(`${name}: variant weights must sum to 100`);
    }
    let order;
    try {
        order = g.topologicalSort().map((i) => names[i]);
    } catch {
        throw new Error("prerequisite cycle");
    }
    const canonical = StableStringify(cfg);
    return { flags: cfg.flags, order, etag: `"${SHA256Hex(canonical).slice(0, 32)}"`, canonical };
}

// =============================================================================
// Evaluation
// =============================================================================
const bucket = (salt, user) => XXHash32(`${salt}:${user.id}`) % 10_000;
function matches(when, user) {
    return Object.entries(when).every(([attr, want]) => {
        if (attr === "app_version") return semver.satisfies(user.app_version, want);
        const have = attr === "email_domain" ? user.email.split("@")[1] : user[attr];
        return Array.isArray(want) ? want.includes(have) : have === want;
    });
}
function evaluate(config, user) {
    const out = {};
    for (const name of config.order) {
        const f = config.flags[name];
        if (f.killed) {
            out[name] = { value: "off", reason: "killed" };
            continue;
        }
        if (f.requires && out[f.requires.flag].value !== f.requires.variant) {
            out[name] = { value: "off", reason: "prerequisite" };
            continue;
        }
        const b = bucket(f.salt, user);
        if (f.variants) {
            let acc = 0;
            for (const [v, w] of Object.entries(f.variants)) {
                acc += w * 100;
                if (b < acc) {
                    out[name] = { value: v, reason: "variant" };
                    break;
                }
            }
            continue;
        }
        let rollout = f.rollout ?? 0,
            reason = "rollout";
        for (const r of f.rules ?? []) {
            if (!matches(r.when, user)) continue;
            if (r.serve) {
                out[name] = { value: r.serve, reason: "rule" };
                rollout = null;
            } else {
                rollout = r.rollout;
                reason = "rule-rollout";
            }
            break;
        }
        if (rollout !== null) out[name] = { value: b < rollout * 100 ? "on" : "off", reason };
    }
    return out;
}

// =============================================================================
// Statistics over 50 000 users
// =============================================================================
const COUNTRIES = ["US", "DE", "FR", "GB", "AT", "CH", "JP", "IN"];
const users = Array.from({ length: 50_000 }, (_, i) => ({
    id: `user-${i}`,
    country: COUNTRIES[i % 8],
    email: i % 50 === 0 ? `dev${i}@example.com` : `u${i}@mail.test`,
    app_version: ["3.1.4", "3.2.0", "3.3.1", "2.9.9"][i % 4],
    plan: i % 3 ? "free" : "pro",
}));
const cfg = load(CONFIG_V1);
const evals = users.map((u) => evaluate(cfg, u));
const share = (pred, flag, value = "on") => {
    const sel = users.map((u, i) => [u, evals[i]]).filter(([u]) => pred(u));
    return sel.filter(([, e]) => e[flag].value === value).length / sel.length;
};
const isDach = (u) => ["DE", "AT", "CH"].includes(u.country),
    isStaff = (u) => u.email.endsWith("@example.com");
results["staff always get new-checkout"] = share(isStaff, "new-checkout") === 1;
results["DACH users get the 50% rollout"] =
    Math.abs(share((u) => isDach(u) && !isStaff(u), "new-checkout") - 0.5) < 0.015;
results["everyone else gets the 20% rollout"] =
    Math.abs(share((u) => !isDach(u) && !isStaff(u), "new-checkout") - 0.2) < 0.01;
results["dark mode follows the semver rule exactly"] = users.every(
    (u, i) => (evals[i]["dark-mode"].value === "on") === semver.satisfies(u.app_version, ">=3.2.0"),
);
results["killed flag is off for everyone"] = evals.every(
    (e) => e["legacy-export"].value === "off" && e["legacy-export"].reason === "killed",
);
{
    const v = new Multiset();
    for (const e of evals) v.add(e["search-v2"].value);
    results["variant weights 50/30/20 within 1 point"] = [
        ["control", 0.5],
        ["bm25", 0.3],
        ["vector", 0.2],
    ].every(([k, w]) => Math.abs(v.count(k) / users.length - w) < 0.01);
    const vr = evals.filter((e) => e["vector-reranker"].value === "on");
    results["prerequisite: reranker only where search-v2 = vector"] = vr.every(
        (e) => e["search-v2"].value === "vector",
    );
    results["reranker reaches ~60% of the vector arm"] = Math.abs(vr.length / v.count("vector") - 0.6) < 0.02;
}
{
    // Chi-square over 100 equal bucket ranges: uniform hashing gives p well above 0.01.
    const counts = new Float64Array(100);
    for (const u of users) counts[Math.floor(bucket("nc-2026", u) / 100)]++;
    const expected = users.length / 100;
    const chi2 = counts.reduce((s, c) => s + (c - expected) ** 2 / expected, 0);
    const p = gammainc(chi2 / 2, 99 / 2, "upper");
    results["bucket distribution passes a chi-square uniformity test"] = p > 0.01;
    const a = users.map((u) => (bucket("nc-2026", u) < 2000 ? 1 : 0)),
        b = users.map((u) => (bucket("dm", u) < 5000 ? 1 : 0));
    const ma = a.average(),
        mb = b.average();
    const cov = a.reduce((s, x, i) => s + (x - ma) * (b[i] - mb), 0) / a.length;
    const corr = cov / Math.sqrt(ma * (1 - ma) * mb * (1 - mb));
    results["two flags' assignments are uncorrelated"] = Math.abs(corr) < 0.02;
    print(`chi-square ${chi2.toFixed(1)} on 99 df, p = ${p.toFixed(3)}; cross-flag correlation ${corr.toFixed(4)}`);
}
{
    const v2 = load(CONFIG_V1.replace("    rollout: 20\n", "    rollout: 50\n"));
    let kept = true,
        added = 0;
    users.forEach((u, i) => {
        const before = evals[i]["new-checkout"].value,
            after = evaluate(v2, u)["new-checkout"].value;
        if (before === "on" && after !== "on") kept = false;
        if (before === "off" && after === "on") added++;
    });
    results["raising a rollout only adds users (sticky bucketing)"] = kept && added > 0;
    let cycle = null;
    try {
        load(
            CONFIG_V1.replace(
                "    salt: s2-2026\n",
                "    salt: s2-2026\n    requires: { flag: vector-reranker, variant: on }\n",
            ),
        );
    } catch (e) {
        cycle = e.message;
    }
    results["a prerequisite cycle is refused at load"] = cycle === "prerequisite cycle";
    let badWeights = null;
    try {
        load(CONFIG_V1.replace("vector: 20", "vector: 25"));
    } catch (e) {
        badWeights = e.message;
    }
    results["variant weights not summing to 100 are refused"] = /sum to 100/.test(badWeights ?? "");
}

// =============================================================================
// Delivery over HTTP with ETags
// =============================================================================
let live = cfg;
const app = new App({ port: 0 });
app.get("/flags", (req) =>
    req.headers["if-none-match"] === live.etag
        ? { status: 304, body: "" }
        : { headers: { etag: live.etag }, contentType: "application/json", body: live.canonical },
);
app.get("/evaluate", (req) => {
    const u = users[Number(req.query.user)];
    return u ? evaluate(live, u) : { status: 404, body: "no such user" };
});
app.start();
const base = `http://127.0.0.1:${app.port}`;
const first = await fetch(base + "/flags");
const etag = first.headers.get("etag");
const second = await fetch(base + "/flags", { headers: { "if-none-match": etag } });
live = load(CONFIG_V1.replace("vector: 20 }", "vector: 20 }\n    note: tuned"));
const third = await fetch(base + "/flags", { headers: { "if-none-match": etag } });
results["config is served with an ETag"] = first.status === 200 && etag === cfg.etag;
results["unchanged config answers 304"] = second.status === 304;
results["a changed config gets a new ETag and a 200"] = third.status === 200 && third.headers.get("etag") !== etag;
const remote = await (await fetch(`${base}/evaluate?user=4242`)).json();
results["remote evaluation equals local evaluation"] =
    JSON.stringify(remote) === JSON.stringify(evaluate(live, users[4242]));
app.close();

print(
    Table(
        Object.keys(cfg.flags).map((name) => {
            const vals = new Multiset();
            for (const e of evals) vals.add(e[name].value);
            return [
                name,
                vals
                    .entrySet()
                    .sort()
                    .map(([k, n]) => `${k} ${((100 * n) / users.length).toFixed(1)}%`)
                    .join(", "),
            ];
        }),
        { head: ["flag", "assignment across 50 000 users"] },
    ),
);

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
