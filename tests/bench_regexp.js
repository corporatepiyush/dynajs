const SCALE = parseFloat(scriptArgs[1] || "1");

function mkNarrow(n) {
    let parts = [];
    for (let i = 0; i < n; i++) parts.push("lorem ipsum dolor sit amet " + (i % 997));
    return parts.join(" | ");
}
function mkWide(n) { return mkNarrow(n) + "中"; }

const SUBJ_N = mkNarrow(400);
const SUBJ_W = mkWide(400);
const NEEDLE_LATE_N = SUBJ_N + " ZZTARGETZZ";
const NEEDLE_LATE_W = SUBJ_W + " ZZTARGETZZ";

function ms(f, reps) {
    f(Math.max(1, reps / 20 | 0));
    const t0 = performance.now();
    f(reps);
    return performance.now() - t0;
}
function row(group, name, width, reps, f) {
    const t = ms(f, reps);
    print("#R " + group + " " + name + " " + width + " " + t.toFixed(3) + " " +
          (reps / (t / 1000)).toFixed(0));
    return t;
}

{
    const pats = [["short", /dolor/g], ["long", /lorem ipsum dolor sit amet/g],
                  ["miss", /ZZNOTHEREZZ/g]];
    for (const [nm, re] of pats)
        for (const [w, s] of [["narrow", SUBJ_N], ["wide", SUBJ_W]])
            row("literal", nm, w, 150000 * SCALE, n => {
                for (let i = 0; i < n; i++) { re.lastIndex = 0; re.test(s); }
            });
}

{
    for (const [w, s] of [["narrow", NEEDLE_LATE_N], ["wide", NEEDLE_LATE_W]]) {
        row("scan", "literal_end", w, 30000 * SCALE, n => {
            const re = /ZZTARGETZZ/; for (let i = 0; i < n; i++) re.test(s);
        });
        row("scan", "class_end", w, 300 * SCALE, n => {
            const re = /Z{2}TARGET/; for (let i = 0; i < n; i++) re.test(s);
        });
    }
}

{
    const cases = [["small_range", /[a-z]+/g], ["word", /[a-zA-Z0-9_]+/g],
                   ["digit", /\d+/g], ["negated", /[^aeiou ]+/g],
                   ["wide_range", /[Ā-俿]/g]];
    for (const [nm, re] of cases)
        for (const [w, s] of [["narrow", SUBJ_N], ["wide", SUBJ_W]])
            row("class", nm, w, 900 * SCALE, n => {
                for (let i = 0; i < n; i++) { re.lastIndex = 0; let m, c = 0;
                    while ((m = re.exec(s)) && ++c < 200) {} }
            });
}

{
    const long = "abcdefghijklmnopqrstuvwxyz0123456789".repeat(8);
    const subjN = long + long + " tail";
    const subjW = subjN + "中";
    for (const [w, s] of [["narrow", subjN], ["wide", subjW]]) {
        row("backref", "long", w, 30000 * SCALE, n => {
            const re = /^(.{288})\1/; for (let i = 0; i < n; i++) re.test(s);
        });
        row("backref", "long_i", w, 15000 * SCALE, n => {
            const re = /^(.{288})\1/i; for (let i = 0; i < n; i++) re.test(s);
        });
        row("backref", "short", w, 20 * SCALE, n => {
            const re = /(\w+) \1/; for (let i = 0; i < n; i++) re.test(s);
        });
    }
}

{
    for (const [w, s] of [["narrow", SUBJ_N], ["wide", SUBJ_W]]) {
        row("quant", "greedy_dot", w, 1500 * SCALE, n => {
            const re = /^.*amet/; for (let i = 0; i < n; i++) re.test(s);
        });
        row("quant", "lazy_dot", w, 60000 * SCALE, n => {
            const re = /^.*?amet/; for (let i = 0; i < n; i++) re.test(s);
        });
        row("quant", "alternation", w, 700 * SCALE, n => {
            const re = /(lorem|ipsum|dolor|sit|amet|consectetur)/g;
            for (let i = 0; i < n; i++) { re.lastIndex = 0; let c = 0;
                while (re.exec(s) && ++c < 200) {} }
        });
        row("quant", "nested_group", w, 700 * SCALE, n => {
            const re = /((\w+)\s+(\w+))/g;
            for (let i = 0; i < n; i++) { re.lastIndex = 0; let c = 0;
                while (re.exec(s) && ++c < 200) {} }
        });
    }
}

{
    const many = new RegExp("(" + "(\\w)".repeat(18) + ")");
    for (const [w, s] of [["narrow", SUBJ_N], ["wide", SUBJ_W]])
        row("capture", "many_groups", w, 150 * SCALE, n => {
            for (let i = 0; i < n; i++) many.exec(s);
        });
}

{
    row("adversarial", "nested_quant", "narrow", 10 * SCALE, n => {
        const re = /^(a+)+$/; const s = "a".repeat(20) + "b";
        for (let i = 0; i < n; i++) re.test(s);
    });
    row("adversarial", "alt_backtrack", "narrow", 160 * SCALE, n => {
        const re = /^(a|aa)+$/; const s = "a".repeat(18) + "b";
        for (let i = 0; i < n; i++) re.test(s);
    });
}

{
    function compileBench(name, build, reps) {
        row("compile", name, "n/a", reps, n => {
            for (let i = 0; i < n; i++) new RegExp(build(i));
        });
    }
    compileBench("small", i => "foo" + (i % 7) + "bar", 100000 * SCALE);
    compileBench("big_alternation", i => "(" +
        Array.from({length: 200}, (_, k) => "w" + k + "x" + (i % 3)).join("|") + ")",
        200 * SCALE);
    compileBench("many_quantifiers", i =>
        Array.from({length: 150}, (_, k) => "(a" + (k % 9) + (i % 2) + ")*").join(""),
        1200 * SCALE);
    compileBench("big_class", i => "[" +
        Array.from({length: 120}, (_, k) => String.fromCharCode(0x100 + k * 3)).join("") +
        String.fromCharCode(0x41 + (i % 20)) + "]+", 500 * SCALE);
    compileBench("nested_groups", i =>
        "(".repeat(40) + "a" + (i % 5) + ")".repeat(40), 20000 * SCALE);
    compileBench("unicode_prop", i => "\\p{L}+" + (i % 3), 100000 * SCALE);
}

print("#R DONE");
