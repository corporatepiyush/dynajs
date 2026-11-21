// covers: c:src/builtins/builtin-string.c
// str[i] with an int index inside the string returns the code unit directly, skipping the atom and
// the String.prototype walk. That is unobservable because a string's index properties are own,
// non-writable and non-configurable. Out of range, negative or non-int keys must still reach the
// prototype chain. Oracle: String.prototype.charAt / charCodeAt, a separate path.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_string_index_fast");
let n = 0,
    fails = 0;
const ok = (got, want, what) => {
    n++;
    const g = JSON.stringify(got),
        w = JSON.stringify(want);
    if (g !== w) {
        fails++;
        if (fails <= 20) print("FAIL: " + what + " got " + g + " want " + w);
    }
};

__kitS.section("every unit 0..0xFFFF, Latin-1, wide and sliced strings", () => {
    let bad = 0;
    const all = [];
    for (let c = 0; c <= 0xffff; c++) all.push(String.fromCharCode(c));
    const wide = all.join("");
    const narrow = all.slice(0, 256).join("");
    for (let i = 0; i < wide.length; i++) if (wide[i] !== wide.charAt(i) || wide[i].charCodeAt(0) !== i) bad++;
    for (let i = 0; i < narrow.length; i++) if (narrow[i] !== narrow.charAt(i)) bad++;
    const sl = ("x".repeat(5000) + wide).slice(4000, 4000 + 3000);
    for (let i = 0; i < sl.length; i++) if (sl[i] !== sl.charAt(i)) bad++;
    ok(bad, 0, "every unit 0..0xFFFF, Latin-1, wide and sliced strings");
});
ok(["😀"[0].charCodeAt(0), "😀"[1].charCodeAt(0), "😀"[2]], [0xd83d, 0xde00, undefined], "surrogate halves and the end");
ok(["abc"[3], "abc"[-1], ""[0], "abc"[2 ** 31 - 1]], [undefined, undefined, undefined, undefined], "out of range");
ok(["abc"[1.0], "abc"["1"], "abc"[1.5]], ["b", "b", undefined], "non-int keys");
__kitS.section("own index beats String.prototype; out of range reaches it", () => {
    let gets = 0;
    Object.defineProperty(String.prototype, "1", { get: () => (gets++, "P"), configurable: true });
    Object.defineProperty(String.prototype, "5", { get: () => (gets++, "Q"), configurable: true });
    Object.defineProperty(String.prototype, "-1", { get: () => (gets++, "N"), configurable: true });
    const r = ["abc"[1], "abc"[5], "abc"[-1], "a"[1]];
    delete String.prototype[1];
    delete String.prototype[5];
    delete String.prototype[-1];
    ok(r, ["b", "Q", "N", "P"], "own index beats String.prototype; out of range reaches it");
    ok(gets, 3, "String.prototype getters run only for indices the string lacks");
});
__kitS.section("interpreter loop", () => {
    const s = "hello";
    let k = 0;
    for (let i = 0; i < 1000; i++) k += s[i % 5] === "hello".charAt(i % 5) ? 1 : 0;
    ok(k, 1000, "interpreter loop");
    const f = (x, i) => x[i];
    ok([f("ab", 0), f([7], 0), f(new Uint8Array([9]), 0), f({ 0: "o" }, 0), f("ab", 9)], ["a", 7, 9, "o", undefined], "one site sees strings, arrays, typed arrays, objects");
});
__kitS.section("null[0] still throws", () => {
    let threw = "";
    try {
        null[0];
    } catch (e) {
        threw = e.constructor.name;
    }
    ok(threw, "TypeError", "null[0] still throws");
});
__kitS.end();

print("test_string_index_fast: " + n + " checks, " + fails + " failures");
if (fails) throw new Error("test_string_index_fast: " + fails + " failures");
