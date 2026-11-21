// flags: --std
// tools/js/check-generated.js -- every committed generated table must be exactly what its generator
// emits today. The generators once printed comments and a different layout than the committed
// (comment-free, clang-formatted) sources, so nobody could rerun them: a regenerated header failed
// the nocomment stage and churned every line. Each generator now emits the committed form; this
// regenerates each one and compares it with the committed text, so a hand edit of a table, or a
// generator that drifts, fails here.
//
//   ./dynajs --std tools/js/check-generated.js            compare every target (exit 1 on drift)
//   ./dynajs --std tools/js/check-generated.js selftest   planted drift must be caught
//
// A target is a whole file, or the region of a file that starts at a line matching `start` and
// runs for as many lines as the generator emits. gen-charsets reads Python's single-byte codec
// tables (its data, not code): without them it is reported SKIPPED, never silently passed.
// gen-textdec-index reads WPT, x/text and iconv the same way (WPT_DIR, GO_XTEXT_DIR): SKIPPED without them.
import * as std from "std";
import { generate as genCrc } from "./gen-crc.js";
import { generate as genGrisu } from "./gen-dtoa-grisu.js";
import { generate as genCharsets } from "./gen-charsets.js";
import { generate as genTextdec } from "./gen-textdec-index.js";

const TARGETS = [
    { gen: "gen-crc", file: "src/core/dyn-hash.c", start: /^static const uint32_t crc32_ieee_s8\[8\]\[256\] = \{$/, run: genCrc },
    { gen: "gen-dtoa-grisu", file: "src/dtoa.c", start: /^#define GRISU_ALPHA_MIN /, run: genGrisu },
    { gen: "gen-charsets", file: "src/dyna-charsets.h", start: null, run: genCharsets },
    { gen: "gen-textdec-index", file: "src/core/dyn-textdec-index.h", start: null, run: genTextdec },
];

export function compare(committed, start, generated) {
    const have = committed.split("\n");
    const want = generated.split("\n");
    if (want.length && want[want.length - 1] === "") want.pop();
    let from = 0;
    if (start) {
        from = have.findIndex((l) => start.test(l));
        if (from < 0) return { error: "the generated region's first line is not in the file" };
    } else if (have.length && have[have.length - 1] === "") {
        have.pop();
    }
    if (!start && have.length !== want.length)
        return { error: `the file has ${have.length} lines, the generator emits ${want.length}`, line: Math.min(have.length, want.length) + 1 };
    for (let i = 0; i < want.length; i++) {
        if (have[from + i] !== want[i])
            return { error: `differs from the generator: committed ${JSON.stringify(have[from + i] ?? "<eof>")}, generated ${JSON.stringify(want[i])}`, line: from + i + 1 };
    }
    return { lines: want.length, line: from + 1 };
}

function check() {
    let bad = 0;
    for (const t of TARGETS) {
        let text;
        try {
            text = t.run();
        } catch (e) {
            if (/encodings directory|textdec index sources missing/.test(String(e))) {
                print(`check-generated: SKIPPED ${t.file}: ${e.message} -- ${t.gen} is NOT proven`);
                continue;
            }
            print(`check-generated: FAIL ${t.gen}: ${e.message}`);
            bad++;
            continue;
        }
        const committed = std.loadFile(t.file);
        if (committed === null) {
            print(`check-generated: FAIL ${t.file} is missing`);
            bad++;
            continue;
        }
        const r = compare(committed, t.start, text);
        if (r.error) {
            print(`check-generated: FAIL ${t.file}${r.line ? ":" + r.line : ""}: ${r.error}; regenerate with tools/js/${t.gen}.js`);
            bad++;
        } else {
            print(`check-generated: ${t.file}:${r.line} matches tools/js/${t.gen}.js (${r.lines} lines)`);
        }
    }
    return bad ? 1 : 0;
}

function selftest() {
    let bad = 0;
    const expect = (name, r, ok) => {
        if (!!r.error === ok) {
            print(`selftest: ${name}: ${JSON.stringify(r)}`);
            bad++;
        }
    };
    const gen = "static const int t[2] = {\n    1,\n    2,\n};\n";
    const file = "int a;\n\n" + gen + "\nint b;\n";
    const start = /^static const int t\[2\] = \{$/;
    expect("an embedded region equal to the generator passes", compare(file, start, gen), true);
    expect("a hand-edited value in the region is caught", compare(file.replace("    2,", "    3,"), start, gen), false);
    expect("a region cut short is caught", compare("int a;\nstatic const int t[2] = {\n    1,\n", start, gen), false);
    expect("a missing region is caught", compare("int a;\n", start, gen), false);
    expect("a whole file equal to the generator passes", compare(gen, null, gen), true);
    expect("a whole file with an extra line is caught", compare(gen + "int x;\n", null, gen), false);
    expect("a generator that emits a comment no longer matches the committed form", compare(gen, null, "/* x */\n" + gen), false);
    print(bad ? `check-generated selftest: ${bad} problem(s)` : "check-generated selftest: drift, truncation and extra lines are caught");
    return bad ? 1 : 0;
}

if (/check-generated\.js$/.test(scriptArgs[0] || "")) std.exit(scriptArgs[1] === "selftest" ? selftest() : check());
