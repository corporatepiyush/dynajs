// tests/lib/perf.js -- the gate for benchmark assertions (CLAUDE.md rule 13).
// The docker linux legs check compatibility, not speed: an emulated or shared
// container skews every cost ratio (M1a-08 measured 16 ns vs 82 ns per cell
// there, and is green on the host). docker/build-and-test.sh exports
// DYNA_COMPAT_ONLY=1 into every container it runs; under it perfCheck prints
// a SKIP line instead of asserting, so a perf regression is caught by the
// host legs only. tools/repo-rules.js (tests-perf-gated) fires on a
// relative-cost assertion in a timing suite that does not go through
// perfCheck; a comparison that is not about speed says `not perf:` on its line.
// A kit case whose only checks are skipped perfChecks ends as a counted SKIP
// (kit reads the kit.perfSkip symbol); passing the case's skip as the 4th
// argument ends it at once. A sloppy-mode script loads this
// with import() (tests/core/test_w3_parser.js).
import { getEnv } from "dyna:sys";

export const compatOnly = getEnv("DYNA_COMPAT_ONLY") === "1";

export function perfCheck(ok, cond, what, skip) {
    if (skip !== undefined && typeof skip !== "function") throw new TypeError("perfCheck: the 4th argument is the kit case's skip function, got " + typeof skip + " (fold extra detail into what)");
    if (compatOnly) {
        if (skip) skip("perf: " + what + " -- DYNA_COMPAT_ONLY=1 (docker legs check compatibility, not speed)");
        print("SKIP perf: " + what + " -- DYNA_COMPAT_ONLY=1 (docker legs check compatibility, not speed)");
        globalThis[Symbol.for("kit.perfSkip")] ??= "perf: " + what + " -- DYNA_COMPAT_ONLY=1 (docker legs check compatibility, not speed)";
        return;
    }
    ok(cond, what);
}
