// covers: dyna:structures.Trie
// Trie#hasPrefix(p) against a Set model (some key starts with p), over random insert/delete
// churn, tail-compressed keys, the empty prefix and multi-byte keys. delete() unlinks the
// branch a removed key leaves empty, so hasPrefix stays one O(len p) walk after deletes.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_trie_has_prefix");
import { Trie } from "dyna:structures";
import { Random } from "dyna:random";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
const model = (set, p) => { for (const k of set) if (k.startsWith(p)) return true; return false; };

__kitS.section("fixed cases", () => {
    const t = new Trie();
    ok(t.hasPrefix("") === false, "empty trie, empty prefix");
    ok(t.hasPrefix("a") === false, "empty trie");
    t.insert("abc");
    ok(t.hasPrefix("") && t.hasPrefix("a") && t.hasPrefix("ab") && t.hasPrefix("abc"), "tail prefixes");
    ok(!t.hasPrefix("abcd") && !t.hasPrefix("abd") && !t.hasPrefix("b"), "tail non-prefixes");
    t.insert("abxyz");
    ok(t.hasPrefix("abx") && t.hasPrefix("abxyz") && !t.hasPrefix("abxz"), "split tail");
    t.delete("abxyz");
    ok(!t.hasPrefix("abx") && t.hasPrefix("ab") && t.hasPrefix("abc"), "deleted branch is gone");
    t.delete("abc");
    ok(!t.hasPrefix("a") && !t.hasPrefix("") && t.size === 0, "all deleted");
    t.insert("a").insert("abcdef");
    t.delete("abcdef");
    ok(t.hasPrefix("a") && !t.hasPrefix("ab"), "deleting a key under a kept key keeps the kept key");
    t.insert("");
    t.delete("a");
    ok(t.hasPrefix("") && !t.hasPrefix("a"), "empty key alone");
    t.insert("héllo").insert("h€");
    ok(t.hasPrefix("hé") && t.hasPrefix("h€") && !t.hasPrefix("hè"), "multi-byte prefixes");
    ok(t.hasPrefix(12) === false && new Trie().insert("12x").hasPrefix(12) === true, "non-string prefix is stringified like has()");
});

__kitS.section("random churn vs a Set model", () => {
    const rng = new Random(11);
    const alpha = ["a", "b", "c", "é"];
    const word = () => { let s = ""; const n = rng.nextBounded(7); for (let i = 0; i < n; i++) s += alpha[rng.nextBounded(alpha.length)]; return s; };
    for (let round = 0; round < 40; round++) {
        const t = new Trie(), set = new Set();
        for (let step = 0; step < 300; step++) {
            const w = word();
            if (rng.nextBounded(3) === 0) { ok(t.delete(w) === set.delete(w), "delete " + w); }
            else { t.insert(w); set.add(w); }
            const p = word();
            const got = t.hasPrefix(p), want = model(set, p);
            if (got !== want) { ok(false, `round ${round} step ${step}: hasPrefix(${JSON.stringify(p)}) = ${got}, model ${want}`); return; }
            pass++;
        }
        ok(t.size === set.size, "size after churn");
        const keys = t.keysWithPrefix("").sort(), mk = [...set].sort();
        ok(JSON.stringify(keys) === JSON.stringify(mk), "keys survive pruning deletes");
        const back = Trie.deserialize(t.serialize());
        ok(back.size === set.size && [...set].every((k) => back.has(k)), "serialize round trip after pruning deletes");
    }
});

__kitS.end();
print("test_structures_trie_has_prefix: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
