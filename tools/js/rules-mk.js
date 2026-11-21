// tools/js/rules-mk.js -- the one reader of OBJDIR/rules.mk, the makefile ./build.sh writes (one rule per unit:
// `OBJ: SRC`, then tab recipe lines; the compile line is `@CC FLAGS... DEPFLAGS -c -o $@ $<`). DEPFLAGS is every -M flag
// before `-c` (today `-MMD -MP -MF $@.d`; it was `-MMD -MF $@.d`), so a change there no longer hides every rule.
// Users: tools/coverage-map.js (rewrites the rules with coverage flags), tools/module-matrix.js (per-unit commands),
// tools/cc-lines.js (one shell line per unit). selftest(text) checks the format fixtures and, given the real rules.mk,
// that every object of its `all:` line has exactly one parsed compile rule.

const COMPILER = /^\t@?(\S*(?:cc|clang|gcc|clang-\d+|gcc-\d+)) /;
const TAIL = " -c -o $@ $<";

export function compileRules(text) {
    const lines = text.split("\n"), rules = [];
    let off = 0;
    const starts = [];
    for (const l of lines) { starts.push(off); off += l.length + 1; }
    for (let i = 0; i < lines.length; i++) {
        const head = /^(\S+\.o): (\S+\.(?:c|m|S))$/.exec(lines[i]);
        if (!head) continue;
        let j = i + 1;
        while (j < lines.length && lines[j].startsWith("\t") && !COMPILER.test(lines[j])) j++;
        if (j >= lines.length || !lines[j].startsWith("\t") || !lines[j].endsWith(TAIL)) continue;
        const body = lines[j].slice(1).replace(/^@/, "");
        const pre = body.slice(0, body.length - TAIL.length);
        const toks = pre.split(" ");
        let k = toks.length;
        while (k > 0 && (/^-M/.test(toks[k - 1]) || (k > 1 && /^-M[FTQ]$/.test(toks[k - 2])))) k--;
        const cmd = toks.slice(0, k).join(" ").trimEnd(), dep = toks.slice(k).join(" ");
        rules.push({ obj: head[1], src: head[2], cmd, dep, gap: pre.slice(cmd.length, pre.length - dep.length), line: j, start: starts[j], end: starts[j] + lines[j].length, at: lines[j][1] === "@" ? "@" : "" });
    }
    return rules;
}

export function mapCompile(text, fn) {
    const rules = compileRules(text);
    let out = "", pos = 0;
    for (const r of rules) {
        out += text.slice(pos, r.start) + "\t" + r.at + fn(r) + r.gap + r.dep + TAIL;
        pos = r.end;
    }
    return { text: out + text.slice(pos), rules: rules.length };
}

export function allObjects(text) {
    const m = /^all: (.*)$/m.exec(text);
    return m ? m[1].split(/\s+/).filter((s) => s.endsWith(".o")) : [];
}

const FIXTURES = [
    [".obj/release/src/a.o: src/a.c\n\t@mkdir -p $(@D)\n\t@echo \"  CC src/a.c\"\n\t@clang -O2 -Isrc -MMD -MF $@.d -c -o $@ $<\n", "clang -O2 -Isrc", "-MMD -MF $@.d"],
    [".obj/release/src/a.o: src/a.c\n\t@mkdir -p $(@D)\n\t@echo \"  CC src/a.c\"\n\t@clang -O2 -Isrc  -MMD -MP -MF $@.d -c -o $@ $<\n", "clang -O2 -Isrc", "-MMD -MP -MF $@.d"],
    [".obj/x/src/a.o: src/a.c\n\t/usr/bin/gcc-14 -O1 -c -o $@ $<\n", "/usr/bin/gcc-14 -O1", ""],
];

export function selftest(real) {
    const bad = [];
    let checks = 0;
    const ok = (c, m) => { checks++; if (!c) bad.push(m); };
    for (const [t, cmd, dep] of FIXTURES) {
        const r = compileRules(t);
        ok(r.length === 1 && r[0].cmd === cmd && r[0].dep === dep && r[0].src === "src/a.c", "fixture parse: " + JSON.stringify(r[0] || null) + " want " + cmd + " | " + dep);
        const m = mapCompile(t, (x) => x.cmd + " -DX");
        ok(m.rules === 1 && m.text.includes(cmd + " -DX" + r[0].gap + dep + TAIL) && m.text.split("\n").length === t.split("\n").length, "fixture rewrite keeps the dep flags and the line count");
    }
    ok(compileRules("a.o: a.c\n\t@echo hi\n").length === 0, "a rule with no compile line is not a compile rule");
    if (real !== null && real !== undefined) {
        const rules = compileRules(real), objs = allObjects(real), seen = new Map();
        for (const r of rules) seen.set(r.obj, (seen.get(r.obj) || 0) + 1);
        ok(rules.length >= 100, "real rules.mk: only " + rules.length + " compile rules parsed");
        ok(objs.length > 0, "real rules.mk: no all: line");
        const missing = objs.filter((o) => seen.get(o) !== 1);
        ok(missing.length === 0, "real rules.mk: " + missing.length + " objects of all: lack exactly one compile rule, e.g. " + missing.slice(0, 3).join(" "));
        ok(rules.every((r) => r.dep === "" || /^-MMD\b/.test(r.dep)), "real rules.mk: a dep-flag tail does not start with -MMD");
        ok(rules.every((r) => !/ -M/.test(" " + r.cmd)), "real rules.mk: a -M flag was left in the compile command");
        const m = mapCompile(real, (x) => x.cmd);
        ok(m.text === real, "real rules.mk: an identity rewrite changes the text");
    }
    return { checks, bad };
}
