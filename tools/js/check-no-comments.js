// flags: --std
// tools/js/check-no-comments.js: the port of tools/check-no-comments.py (deleted); its output was proven identical, apart from naming itself
// Owner directive: engine C sources (src/**/*.c, *.h, *.inc.c) carry ZERO comments.
//
// Scans with a small C lexer (string and character literals are skipped, so "http://x" and '/' are not comments) and
// prints every file:line that opens a // or /* comment.  Before scanning it runs a self-test that plants a comment of
// each kind (and the literal look-alikes) in a scratch file and proves the lexer reports exactly the planted ones: a
// checker that cannot fail proves nothing.
//
//     ./dynajs --std tools/js/check-no-comments.js [ROOT]      exit 0 when clean, 1 with a list when not, 2 when the self-test fails
import * as std from "std";
import { walk, join, exists, relpath, readText, Out } from "./pyshim.js";

function countNl(text, a, b) {
    let n = 0;
    for (let k = text.indexOf("\n", a); k >= 0 && k < b; k = text.indexOf("\n", k + 1)) n++;
    return n;
}

export function commentLines(text) {
    const out = [];
    let i = 0, line = 1;
    const n = text.length;
    while (i < n) {
        const c = text[i];
        if (c === "\n") {
            line++;
            i++;
        } else if (c === '"' || c === "'") {
            const q = c;
            i++;
            while (i < n && text[i] !== q && text[i] !== "\n") {
                if (text[i] === "\\") i++;
                i++;
            }
            i++;
        } else if (text.startsWith("//", i) || text.startsWith("/*", i)) {
            out.push(line);
            if (text.startsWith("//", i)) {
                while (i < n && text[i] !== "\n") i++;
            } else {
                let j = text.indexOf("*/", i + 2);
                j = j < 0 ? n : j + 2;
                line += countNl(text, i, j);
                i = j;
            }
        } else i++;
    }
    return out;
}

function selftest() {
    const sample =
        'const char* u = "http://example/*not*/";\n' +
        "char s = '/';\n" +
        'const char* e = "\\"//still a string\\"";\n' +
        "int a; // planted line comment\n" +
        "/* planted\n   block */ int b;\n" +
        "int c = 1 / 2;\n";
    const got = commentLines(sample);
    return got.length === 2 && got[0] === 4 && got[1] === 5;
}

function main() {
    const o = new Out();
    const root = scriptArgs.length > 1 ? scriptArgs[1] : ".";
    if (!selftest()) {
        o.print("FAIL: check-no-comments self-test: the lexer missed a planted comment or flagged a literal");
        o.flush();
        return 2;
    }
    const files = [];
    for (const [base, , names] of walk(join(root, "src")))
        for (const name of names) if (name.endsWith(".c") || name.endsWith(".h")) files.push(join(base, name));
    files.sort();
    const bad = [];
    for (const path of files)
        for (const ln of commentLines(readText(path, "latin1"))) bad.push(relpath(path, root) + ":" + ln);
    if (bad.length) {
        o.print(`FAIL: ${bad.length} comment(s) in engine sources (owner directive: zero comments):`);
        for (const b of bad.slice(0, 50)) o.print("  " + b);
        if (bad.length > 50) o.print(`  ... and ${bad.length - 50} more`);
        o.flush();
        return 1;
    }
    o.print(`no-comments: ${files.length} engine source files clean (self-test planted 2, caught 2, ignored 3 literals)`);
    o.flush();
    return 0;
}

std.exit(main());
