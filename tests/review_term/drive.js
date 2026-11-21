import * as std from "std";
import { Exec } from "dyna:sys";

export const BIN = std.getenv("DYNAJS") || "./dynajs";
export function run(probe, mode, payload, keep, timeoutMs = 30000) {
    const r = Exec(BIN, ["--std", "tests/review_term/" + probe, mode], { input: payload, timeoutMs, maxBuffer: 64 << 20 });
    if (r.timedOut) return ["<TIMEOUT>"];
    return (String(r.stdout) + String(r.stderr)).split("\n").filter((l) => l.length > 0 && keep(l));
}
export function bytes(...parts) {
    const enc = parts.map((p) => (typeof p === "string" ? new TextEncoder().encode(p) : p));
    const out = new Uint8Array(enc.reduce((n, p) => n + p.length, 0));
    let o = 0;
    for (const p of enc) { out.set(p, o); o += p.length; }
    return out;
}
export const fill = (ch, n) => new Uint8Array(n).fill(ch.charCodeAt(0));
let fails = 0;
export function check(pid, desc, ok, detail = "") {
    print(`${ok ? "PASS" : "FAIL"} ${pid} -- ${desc}${ok ? "" : " :: " + detail}`);
    if (!ok) fails++;
}
export function done(name) {
    print(name + ": done");
    std.exit(fails ? 1 : 0);
}
