// 44 · Dependency resolver — picks package versions that satisfy every semver constraint, in install order.
//
// WHAT IT SHOWS
//   - dyna:semver: ranges, maxSatisfying, and why prereleases need care
//   - backtracking resolution: when the newest version of one package conflicts, try the next
//   - dyna:structures Graph.topologicalSort for install order, and cycle detection
//   - an explanation of WHY resolution failed, which is the part users actually need
//
// RUN      dynajs examples/apps/44-dependency-resolver.js

import { Range, sort as sortVersions, satisfies, maxSatisfying } from "dyna:semver";
import { Graph } from "dyna:structures";

// The registry: package -> version -> its dependencies (name -> range).
const registry = {
    app:      { "1.0.0": { web: "^2.0.0", db: "^1.2.0", log: "*" } },
    web:      { "2.0.0": { http: "^1.0.0" }, "2.1.0": { http: "^1.4.0", log: "^2.0.0" }, "3.0.0-beta.1": { http: "^2.0.0" } },
    db:       { "1.2.0": { pool: "~1.0.0" }, "1.3.0": { pool: "^1.1.0", log: "^1.0.0" } },
    http:     { "1.0.0": {}, "1.4.2": {}, "1.5.0": {}, "2.0.0": {} },
    pool:     { "1.0.3": {}, "1.1.0": {}, "1.2.0": {} },
    log:      { "1.0.0": {}, "1.9.0": {}, "2.0.0": {}, "2.3.1": {} },
};

// Versions of a package that satisfy ALL constraints gathered so far, newest first.
function candidates(name, ranges) {
    const versions = Object.keys(registry[name] ?? {});
    const ok = versions.filter((v) => ranges.every((r) => satisfies(v, r)));
    return sortVersions(ok).reverse();
}

// Depth-first search with backtracking. `chosen` maps package -> version;
// `constraints` maps package -> [{ range, by }] so failures can be explained.
function resolve(root, rootVersion) {
    const failures = [];
    const search = (queue, chosen, constraints) => {
        if (queue.length === 0) return chosen;
        const [name, ...rest] = queue;
        const ranges = constraints.get(name).map((c) => c.range);

        if (chosen.has(name)) {
            // Already picked: it must still satisfy the newly added constraint.
            return ranges.every((r) => satisfies(chosen.get(name), r)) ? search(rest, chosen, constraints) : null;
        }
        const options = candidates(name, ranges);
        if (options.length === 0)
            failures.push(`${name}: no version satisfies ${constraints.get(name).map((c) => `${c.range} (from ${c.by})`).join(" and ")}`);

        for (const version of options) {
            const nextChosen = new Map(chosen).set(name, version);
            const nextConstraints = new Map([...constraints].map(([k, v]) => [k, [...v]]));
            const deps = registry[name][version];
            for (const [dep, range] of Object.entries(deps))
                nextConstraints.set(dep, [...(nextConstraints.get(dep) ?? []), { range, by: `${name}@${version}` }]);
            const solved = search([...rest, ...Object.keys(deps)], nextChosen, nextConstraints);
            if (solved) return solved;                          // first success wins: newest-first order
        }
        return null;                                            // backtrack
    };
    const start = new Map([[root, [{ range: rootVersion, by: "you" }]]]);
    const solution = search([root], new Map(), start);
    return solution ? { ok: true, versions: solution } : { ok: false, reasons: [...new Set(failures)] };
}

// Dependencies must be installed before their dependents.
function installOrder(versions) {
    const names = [...versions.keys()];
    const index = new Map(names.map((n, i) => [n, i]));
    const graph = new Graph({ directed: true });
    for (const _ of names) graph.addNode();
    for (const [name, version] of versions)
        for (const dep of Object.keys(registry[name][version])) graph.addEdge(index.get(dep), index.get(name));
    return graph.topologicalSort().map((i) => `${names[i]}@${versions.get(names[i])}`);   // throws on a cycle
}

// ---- demo ------------------------------------------------------------------
const result = resolve("app", "1.0.0");
console.log("resolved:", [...result.versions].map(([n, v]) => `${n}@${v}`).join(", "));
const order = installOrder(result.versions);
console.log("install order:", order.join(" -> "));

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
const v = result.versions;
check(result.ok && v.get("web") === "2.1.0", "the newest compatible web is chosen, and the prerelease 3.0.0-beta.1 is not");
// web@2.1.0 wants log ^2, db@1.3.0 wants log ^1: they cannot coexist, so the
// resolver must back off db to 1.2.0 (which does not constrain log).
check(v.get("db") === "1.2.0" && v.get("log") === "2.3.1", `a conflict is solved by backtracking (db ${v.get("db")}, log ${v.get("log")})`);
check(v.get("pool") === "1.0.3", "tilde keeps pool within 1.0.x");
check(v.get("http") === "1.5.0", "caret takes the newest 1.x of http");
for (const [name, version] of v)
    for (const [dep, range] of Object.entries(registry[name][version]))
        check(new Range(range).test(v.get(dep)), `${name}@${version} is satisfied by ${dep}@${v.get(dep)}`);
check(order.indexOf("http@1.5.0") < order.indexOf("web@2.1.0") && order[order.length - 1] === "app@1.0.0", "dependencies install before dependents");

// Prereleases only match a range that names a prerelease at the same version.
check(maxSatisfying(Object.keys(registry.web), "^2.0.0 || ^3.0.0") === "2.1.0", "a plain range ignores prereleases");
check(maxSatisfying(Object.keys(registry.web), ">=3.0.0-beta.0") === "3.0.0-beta.1", "an explicit prerelease range admits them");

// An impossible request is explained, not just refused.
registry.app["2.0.0"] = { web: "^2.1.0", log: "^1.0.0" };
const impossible = resolve("app", "2.0.0");
check(!impossible.ok && impossible.reasons.some((r) => r.startsWith("log:") && r.includes("web@2.1.0")), "the failure names the conflicting requirements");
console.log("unsatisfiable example:", impossible.reasons[0]);

// A dependency cycle is caught at ordering time.
registry.http["1.5.0"] = { web: "*" };
let cycle = "";
try { installOrder(v); } catch (e) { cycle = e.message; }
check(cycle.includes("cycle"), "a dependency cycle is reported");
console.log("self-test passed");
