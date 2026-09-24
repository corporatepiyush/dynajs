/* bench_validate.js — the dyna:validate predicates, ns/op over reps.
 * Cross-module validators (IsURL/IsDomain/IsUUID/IsSemver) call the exporting
 * module's JS entry point once per call; that is the design, not overhead.
 * Run: dynajs tests/bench_validate.js */
import { IsURL, IsDomain, IsSlug, IsUUID, IsJWT, IsSemver, IsE164 }
    from "dyna:validate";

const V = {
    IsURL: "https://example.com/path?q=1#f",
    IsDomain: "sub.example.co.uk",
    IsSlug: "hello-world",
    IsUUID: "123e4567-e89b-12d3-a456-426614174000",
    IsJWT: "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.dGVzdHNpZ25hdHVyZQ",
    IsSemver: "1.2.3-rc.1+build.2",
    IsE164: "+14155552671",
};

function bench(fn, iters) {
    for (let w = 0; w < 3; w++) fn(2000);
    let best = Infinity;
    for (let r = 0; r < 5; r++) {
        const t0 = performance.now();
        fn(iters);
        const dt = performance.now() - t0;
        if (dt < best) best = dt;
    }
    return best;
}

print("=== dyna:validate ns/op (best of 5, warmed) ===");
for (const [name, vec] of Object.entries(V)) {
    const f = { IsURL, IsDomain, IsSlug, IsUUID, IsJWT, IsSemver, IsE164 }[name];
    const iters = 2e5;
    const ms = bench((k) => { let s = 0; for (let i = 0; i < k; i++) if (f(vec)) s++; return s; }, iters);
    print("bench_validate " + name.padEnd(10) + (ms * 1e6 / iters).toFixed(1) + " ns/op");
}
print("done.");
