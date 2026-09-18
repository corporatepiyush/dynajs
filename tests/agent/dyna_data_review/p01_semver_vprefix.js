import { parse, isValid, compare, eq, satisfies, sort, prerelease, clean } from "dyna:semver";
import { eq as EQ, done as DONE } from "./harness.js";

// the split grammar: version-side 'v' was rejected while range-side accepted it
EQ(parse("v1.2.3").version, "1.2.3", "parse accepts v-prefix");
EQ(parse("v1.2.3-rc.1").version, "1.2.3-rc.1", "parse v-prefix + prerelease");
EQ(isValid("v1.2.3"), true, "isValid v-prefix");
EQ(compare("v1.2.3", "1.2.3"), 0, "compare v vs plain");
EQ(eq("v1.2.3", "1.2.3"), true, "eq v vs plain");
EQ(satisfies("v1.2.3", ">=1.0.0"), true, "satisfies v-version vs plain range");
EQ(satisfies("1.2.3", ">=v1.0.0"), true, "satisfies plain version vs v-range");
EQ(clean("v1.2.3"), "1.2.3", "clean still strips v");

// over-acceptance guards: exactly ONE leading v, still strict after it
EQ(isValid("vv1.2.3"), false, "double v rejected");
EQ(isValid("V1.2.3"), false, "uppercase V rejected");
EQ(isValid("v1.2"), false, "partial version after v rejected");
EQ(isValid("v"), false, "bare v rejected");
EQ(isValid(" v1.2.3"), false, "space before v rejected");
EQ(isValid("v01.2.3"), false, "leading zero after v rejected");
EQ(isValid("v1.2.3-beta.01"), false, "leading-zero prerelease id after v rejected");
EQ(isValid(""), false, "empty rejected");

// sort/prerelease/v interplay
EQ(sort(["v1.0.0", "1.0.0"]), ["v1.0.0", "1.0.0"], "sort handles mixed v/plain");
EQ(prerelease("v1.2.3-rc.1"), ["rc", 1], "prerelease of v-prefixed");
EQ(parse("v1.2.3").major, 1, "major of v-prefixed");
DONE("p01_semver_vprefix");
