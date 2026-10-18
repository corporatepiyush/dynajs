import * as file from "dyna:file";

const USE_PATH = typeof file.Path === "function";
const mk = (root, name) => USE_PATH ? root.join(name) : root + "/" + name;

const root = file.makeTempDir("dyna-path-ab-");
const p = mk(root, "f.txt");
file.writeFile(p, "x".repeat(256));

function T(fn, n) {
    for (let i = 0; i < Math.min(n, 20000); i++) fn();
    const t = performance.now();
    for (let i = 0; i < n; i++) fn();
    return (performance.now() - t) * 1000 / n;
}

const tag = USE_PATH ? "NEW(Path)" : "OLD(string)";
print(`#AB\t${tag}\texists\t${T(() => file.exists(p), 200000).toFixed(3)}`);
print(`#AB\t${tag}\tstat\t${T(() => file.stat(p), 100000).toFixed(3)}`);
print(`#AB\t${tag}\treadFile\t${T(() => file.readFile(p), 100000).toFixed(3)}`);

file.removeAll(root);
