// covers: c:src/builtins/builtin-regexp.c
// new RegExp(re, flags) shares re's compiled program when the flags equal re's own; matchAll and
// any species clone depend on that. The program must stay the one compiled for the pattern it reports,
// even when user code (flags.toString, a prototype getter) recompiles re in the middle of the clone.
import { sections as __kitSections } from "../lib/kit.js";
import { perfCheck } from "../lib/perf.js";
const __kitS = __kitSections("test_regexp_clone_bytecode");
let n = 0,
    fails = 0;
const ok = (got, want, what) => {
    n++;
    const g = JSON.stringify(got),
        w = JSON.stringify(want);
    if (g !== w) {
        fails++;
        print("FAIL: " + what + " got " + g + " want " + w);
    }
};

__kitS.section("same flags: source and flags carried", () => {
    const re = /a(?<x>b)/gi;
    const c = new RegExp(re, "gi");
    ok([c.source, c.flags, c.lastIndex], ["a(?<x>b)", "gi", 0], "same flags: source and flags carried");
    ok(c.exec("xxAB").groups.x, "B", "same flags: named groups and ignoreCase still compiled in");
    ok(c !== re, true, "same flags: a distinct object");
    re.lastIndex = 3;
    ok(new RegExp(re, "gi").lastIndex, 0, "the clone starts at lastIndex 0");
});
__kitS.section("different flags recompile: i takes effect", () => {
    const re = /a/g;
    ok(new RegExp(re, "i").test("A"), true, "different flags recompile: i takes effect");
    ok(new RegExp(re, "gy").sticky, true, "different flags: sticky");
    ok(new RegExp(/a/gi, "ig").flags, "gi", "flag order does not matter");
    let threw = "";
    try {
        new RegExp(re, "gg");
    } catch (e) {
        threw = e.name;
    }
    ok(threw, "SyntaxError", "duplicate flags are still refused");
    try {
        threw = "";
        new RegExp(/a/u, "uv");
    } catch (e) {
        threw = e.name;
    }
    ok(threw, "SyntaxError", "u with v is still refused");
});
__kitS.section("toString recompiles re mid-clone", () => {
    // flags.toString recompiles the source regexp: the clone keeps the OLD pattern with the OLD program
    const re = /a/g;
    const flags = {
        toString() {
            re.compile("b", "g");
            return "g";
        },
    };
    const c = new RegExp(re, flags);
    ok([c.source, c.test("a"), new RegExp(c.source, "g").test("b")], ["a", true, false], "toString recompiles re mid-clone");
    ok([re.source, re.test("b")], ["b", true], "re itself was recompiled");
});
{
    // the same through the prototype read on new.target
    const re = /x+/g;
    function F() {}
    const nt = new Proxy(F, {
        get(t, k) {
            if (k === "prototype") re.compile("y", "g");
            return Reflect.get(t, k);
        },
    });
    const c = Reflect.construct(RegExp, [re, "g"], nt);
    const source = Object.getOwnPropertyDescriptor(RegExp.prototype, "source").get;
    ok([source.call(c), RegExp.prototype.test.call(c, "xx"), RegExp.prototype.test.call(c, "y")], ["x+", true, false], "prototype getter recompiles re mid-clone");
}
__kitS.section("Café crème brûlée, 3-in-1 gore-tex", () => {
    const s = "Café crème brûlée, 3-in-1 gore-tex";
    const re = /[\p{L}\p{N}]+(?:-[\p{L}\p{N}]+)*/gu;
    ok([...s.matchAll(re)].map((m) => m[0]), ["Café", "crème", "brûlée", "3-in-1", "gore-tex"], "matchAll with unicode classes");
    ok(re.lastIndex, 0, "matchAll leaves the source regexp alone");
    class R extends RegExp {}
    const sub = new R("o", "g");
    ok([..."foo".matchAll(sub)].length, 2, "a subclass clones through its own species");
});
__kitS.section("The Hoka Alpine rain jacket is waterproof, gore-tex, hooded and built for long distances.", () => {
    // the program is shared, not recompiled: matchAll with large Unicode classes used to cost a compile per call
    // (14x an exec loop); interleaved min-of-5 with wide headroom
    const text = "The Hoka Alpine rain jacket is waterproof, gore-tex, hooded and built for long distances.";
    const re = /[\p{L}\p{N}]+(?:-[\p{L}\p{N}]+)*/gu;
    const viaMatchAll = () => {
        let k = 0;
        for (let i = 0; i < 300; i++) for (const m of text.matchAll(re)) k++;
        return k;
    };
    const viaExec = () => {
        let k = 0;
        for (let i = 0; i < 300; i++) {
            const r = new RegExp(re.source, "gu");
            while (r.exec(text)) k++;
        }
        return k;
    };
    const viaExecShared = () => {
        let k = 0;
        for (let i = 0; i < 300; i++) {
            re.lastIndex = 0;
            while (re.exec(text)) k++;
        }
        return k;
    };
    let a = Infinity,
        b = Infinity;
    for (let r = 0; r < 5; r++) {
        let t = performance.now();
        viaMatchAll();
        a = Math.min(a, performance.now() - t);
        t = performance.now();
        viaExecShared();
        b = Math.min(b, performance.now() - t);
    }
    ok(viaMatchAll(), viaExec(), "matchAll and exec agree");
    n++;
    perfCheck((c, what) => { if (!c) { fails++; print("FAIL: " + what); } }, a < b * 4, `matchAll costs under 4x a shared exec (${a.toFixed(2)} ms vs ${b.toFixed(2)} ms; slower means the clone recompiles)`);
});
__kitS.end();
print("test_regexp_clone_bytecode: " + n + " checks, " + fails + " failures");
if (fails) throw new Error("test_regexp_clone_bytecode: " + fails + " failures");
