import { prerelease, parse } from "dyna:semver";
import { eq as EQ, done as DONE } from "./harness.js";

// prerelease(v) -> null when none (documented), [] would violate
EQ(prerelease("1.2.3"), null, "prerelease none -> null (was [])");
EQ(prerelease("v1.2.3"), null, "prerelease v-prefixed none -> null");
// identifiers
EQ(prerelease("1.2.3-alpha"), ["alpha"], "single alpha id");
EQ(prerelease("1.2.3-alpha.1"), ["alpha", 1], "numeric id is a NUMBER");
EQ(prerelease("1.2.3-rc.1+build.5"), ["rc", 1], "build ignored");
EQ(prerelease("1.2.3-0"), [0], "zero id");
EQ(prerelease("1.2.3-0abc"), ["0abc"], "zero-lead non-numeric id stays string");
// parse().prerelease REMAINS [] for none (object-field form unchanged)
EQ(parse("1.2.3").prerelease, [], "parse().prerelease none stays []");
EQ(parse("1.2.3-alpha").prerelease, ["alpha"], "parse().prerelease some");
EQ(parse("1.2.3").build, [], "parse().build none stays []");
// invalid inputs still throw
TH(function () { prerelease("banana"); }, "TypeError", "prerelease invalid throws");
TH(function () { prerelease("1.2"); }, "TypeError", "prerelease partial throws");
DONE("p05_semver_prerelease");
