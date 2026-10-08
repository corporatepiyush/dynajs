// 35 · Deployment manifest linter — checks multi-document YAML against policy and reports by document and path.
//
// WHAT IT SHOWS
//   - dyna:yaml ParseStream: documents one at a time, each error naming its document and line
//   - dyna:schema for shape, plus hand-written rules for policy a schema cannot express
//   - dyna:json Pointer paths in findings, so a report points at the exact field
//   - exit codes suitable for CI: non-zero when any error-level finding exists
//
// RUN      dynajs examples/apps/35-deployment-manifest-linter.js [manifest.yaml ...]

import { ParseStream } from "dyna:yaml";
import { Schema } from "dyna:schema";
import { Pointer } from "dyna:json";
import { Path, readFile } from "dyna:file";

const shape = Schema.compile({
    type: "object",
    required: ["kind", "metadata", "spec"],
    properties: {
        kind: { enum: ["Deployment", "Service"] },
        metadata: { type: "object", required: ["name"], properties: { name: { type: "string", pattern: "^[a-z0-9-]{1,63}$" } } },
        spec: { type: "object" },
    },
});

// Policy rules: each returns findings { level, path, message }.
const rules = [
    function imagesArePinned(doc) {
        return containers(doc).flatMap(([c, path]) => {
            const image = String(c.image ?? "");
            if (!image) return [{ level: "error", path: path + "/image", message: "container has no image" }];
            if (!image.includes(":") || image.endsWith(":latest"))
                return [{ level: "error", path: path + "/image", message: `image "${image}" is not pinned to a version` }];
            return [];
        });
    },
    function resourceLimitsAreSet(doc) {
        return containers(doc).flatMap(([c, path]) =>
            ["memory", "cpu"].filter((r) => !Pointer.has(c, "/resources/limits/" + r))
                .map((r) => ({ level: "error", path: `${path}/resources/limits/${r}`, message: `no ${r} limit: one runaway pod can starve the node` })));
    },
    function noPrivilegedContainers(doc) {
        return containers(doc).filter(([c]) => c.securityContext?.privileged === true)
            .map(([, path]) => ({ level: "error", path: path + "/securityContext/privileged", message: "privileged containers are not allowed" }));
    },
    function replicasForAvailability(doc) {
        if (doc.kind !== "Deployment") return [];
        const n = doc.spec.replicas ?? 1;
        return n < 2 ? [{ level: "warning", path: "/spec/replicas", message: `only ${n} replica: a node drain causes downtime` }] : [];
    },
    function noSecretsInEnv(doc) {
        return containers(doc).flatMap(([c, path]) => (c.env ?? []).flatMap((e, i) =>
            /(password|secret|token|key)/i.test(e.name ?? "") && typeof e.value === "string"
                ? [{ level: "error", path: `${path}/env/${i}/value`, message: `"${e.name}" is a literal secret; reference a Secret instead` }] : []));
    },
];

function containers(doc) {
    const list = Pointer.has(doc, "/spec/template/spec/containers") ? Pointer.get(doc, "/spec/template/spec/containers") : [];
    return Array.isArray(list) ? list.map((c, i) => [c, `/spec/template/spec/containers/${i}`]) : [];
}

function lint(text, file = "<input>") {
    const findings = [];
    let index = 0;
    try {
        // Documents are parsed lazily: a syntax error in document 3 still lets
        // documents 1 and 2 be linted, and the error names document 3.
        for (const doc of { [Symbol.iterator]: () => ParseStream(text) }) {
            index++;
            if (doc === null) continue;                                    // an empty document between markers
            const name = doc?.metadata?.name ?? `document ${index}`;
            const verdict = shape.validate(doc);
            if (!verdict.valid) {
                findings.push({ file, doc: name, level: "error", path: verdict.errors[0].instancePath || "/", message: "shape: " + verdict.errors[0].keyword });
                continue;                                                  // policy rules assume the shape
            }
            for (const rule of rules) for (const f of rule(doc)) findings.push({ file, doc: name, ...f });
        }
    } catch (e) {
        findings.push({ file, doc: `document ${index + 1}`, level: "error", path: "/", message: "syntax: " + e.message });
    }
    return findings;
}

// ---- command line / self-test ---------------------------------------------
const files = scriptArgs.slice(1);
if (files.length) {
    const all = files.flatMap((f) => lint(readFile(new Path(f)), f));
    for (const f of all) console.log(`${f.level.toUpperCase().padEnd(7)} ${f.file} [${f.doc}] ${f.path}: ${f.message}`);
    const errors = all.filter((f) => f.level === "error").length;
    console.log(`${errors} error(s), ${all.length - errors} warning(s)`);
    if (errors) throw new Error("lint failed");                            // non-zero exit for CI
} else {
    const manifest = `
kind: Deployment
metadata: { name: web }
spec:
  replicas: 3
  template:
    spec:
      containers:
        - name: app
          image: registry.example/web:1.4.2
          resources: { limits: { memory: 256Mi, cpu: 500m } }
---
kind: Deployment
metadata: { name: worker }
spec:
  template:
    spec:
      containers:
        - name: worker
          image: registry.example/worker:latest
          securityContext: { privileged: true }
          env:
            - { name: DB_PASSWORD, value: hunter2 }
            - { name: LOG_LEVEL, value: info }
---
kind: CronJob
metadata: { name: nightly }
spec: {}
---
kind: Service
metadata: { name: Bad_Name }
spec: {}
`;
    const findings = lint(manifest, "sample.yaml");
    for (const f of findings) console.log(`${f.level.toUpperCase().padEnd(7)} [${f.doc}] ${f.path}: ${f.message}`);

    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
    const of = (doc) => findings.filter((f) => f.doc === doc);
    check(of("web").length === 0, "a compliant deployment has no findings");
    const worker = of("worker").map((f) => f.path);
    check(worker.includes("/spec/template/spec/containers/0/image"), "an unpinned image is reported at its path");
    check(worker.includes("/spec/template/spec/containers/0/env/0/value"), "the literal secret is reported; LOG_LEVEL is not");
    check(of("worker").filter((f) => f.level === "warning").length === 1, "single replica is a warning, not an error");
    check(of("worker").length === 6, "worker has six findings: " + of("worker").length);
    check(of("nightly")[0].message.startsWith("shape"), "an unsupported kind fails the shape check");
    check(of("Bad_Name").length === 1, "an invalid name fails the shape check");
    const broken = lint("kind: Deployment\nmetadata: { name: ok }\nspec: {}\n---\nkind: [unclosed\n");
    check(broken.length === 2 && broken[1].message.startsWith("syntax") && broken[1].doc === "document 2",
          "a syntax error names its document and earlier documents are still linted");
    console.log("self-test passed:", findings.length, "findings across 4 documents");
}
