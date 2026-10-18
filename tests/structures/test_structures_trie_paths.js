import { Trie } from "dyna:structures";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }

function agree(keys, probes, label) {
    const t = new Trie(), s = new Set();
    for (const k of keys) { t.insert(k); s.add(k); }
    eq(t.size, s.size, label + ": size");
    let bad = null;
    for (const k of keys) if (!t.has(k)) { bad = k; break; }
    check(bad === null, label + ": lost key " + JSON.stringify(bad));
    for (const p of probes) {
        if (t.has(p) !== s.has(p)) { bad = p; break; }
    }
    check(bad === null || !probes.length,
          label + ": has() disagrees for " + JSON.stringify(bad));
    const all = t.keysWithPrefix("").sort();
    const want = [...s].sort();
    let same = all.length === want.length;
    if (same) for (let i = 0; i < all.length; i++) if (all[i] !== want[i]) { same = false; break; }
    check(same, label + ": enumeration differs -- got " + JSON.stringify(all.slice(0, 6)) +
          " want " + JSON.stringify(want.slice(0, 6)));
    return t;
}

{
    agree(["abcdef"], ["a", "ab", "abcde", "abcdefg", "", "x"], "a single key");

    agree(["abcdef", "axxxxx"], ["a", "ab", "ax", "abcdef", "axxxxx"], "diverge at byte 0");
    agree(["abcdef", "abcxyz"], ["abc", "abcd", "abcx", "ab"], "diverge mid-tail");
    agree(["abcdef", "abcdex"], ["abcde", "abcdef", "abcdex"], "diverge at the last byte");
    agree(["abcdef", "abc"], ["abc", "abcd", "abcdef"], "new key stops inside the tail");
    agree(["abc", "abcdef"], ["abc", "abcd", "abcdef", "abcdefg"], "new key extends the tail");
    agree(["abcdef", "abcdef"], ["abcdef"], "re-inserting the same key");
    agree(["ab", "abcd", "abcdef"], ["a", "ab", "abc", "abcd", "abcde", "abcdef"],
          "a chain of prefixes");
    agree(["abcdef", "abcd", "ab"], ["a", "ab", "abc", "abcd", "abcde", "abcdef"],
          "the same chain, longest first");

    agree(["", "abcdef"], ["", "a", "abcdef"], "the empty key and a tail");
    agree(["abcdef", ""], ["", "abcdef"], "a tail then the empty key");
}

{
    const t = new Trie();
    t.insert("abcdef");
    for (const [p, want] of [["", ["abcdef"]], ["a", ["abcdef"]], ["ab", ["abcdef"]],
                             ["abc", ["abcdef"]], ["abcde", ["abcdef"]],
                             ["abcdef", ["abcdef"]], ["abcdefg", []],
                             ["abx", []], ["b", []]]) {
        const got = t.keysWithPrefix(p).sort();
        eq(got.join("|"), want.join("|"), "keysWithPrefix(" + JSON.stringify(p) + ")");
    }

    const u = new Trie();
    for (const k of ["abcdef", "abcxyz", "ab", "zz"]) u.insert(k);
    for (const [p, want] of [["ab", ["ab", "abcdef", "abcxyz"]],
                             ["abc", ["abcdef", "abcxyz"]],
                             ["abcd", ["abcdef"]],
                             ["abcde", ["abcdef"]],
                             ["abcx", ["abcxyz"]],
                             ["z", ["zz"]],
                             ["abcq", []]]) {
        const got = u.keysWithPrefix(p).sort();
        eq(got.join("|"), want.sort().join("|"),
           "branched keysWithPrefix(" + JSON.stringify(p) + ")");
    }
}

{
    const t = new Trie();
    for (const k of ["ab", "abcdef"]) t.insert(k);
    eq(t.longestPrefix("abcdefgh"), "abcdef", "the tail completes the longer key");
    eq(t.longestPrefix("abcdef"), "abcdef", "an exact tail match");
    eq(t.longestPrefix("abcde"), "ab", "a partial tail falls back to the node key");
    eq(t.longestPrefix("abc"), "ab", "a shorter probe");
    eq(t.longestPrefix("ab"), "ab", "the node key itself");
    eq(t.longestPrefix("a"), "", "nothing matches");
    eq(t.longestPrefix("zz"), "", "a different branch");

    const u = new Trie();
    u.insert("abcdef");
    eq(u.longestPrefix("abcdefgh"), "abcdef", "a lone tail completes");
    eq(u.longestPrefix("abcde"), "", "a partial lone tail matches nothing");
}

{
    const t = new Trie();
    for (const k of ["ab", "abcdef", "abcxyz"]) t.insert(k);
    eq(t.size, 3, "three keys");
    check(t.delete("abcdef"), "deleting a tail key reports success");
    eq(t.size, 2, "size after deleting a tail key");
    check(!t.has("abcdef"), "the tail key is gone");
    check(t.has("abcxyz"), "its sibling survives");
    check(t.has("ab"), "and the node key survives");
    check(!t.delete("abcdef"), "deleting it again reports failure");
    t.insert("abcdef");
    eq(t.size, 3, "re-inserting restores the count");
    check(t.has("abcdef"), "and the key");

    check(t.delete("ab"), "deleting a node key");
    eq(t.size, 2, "size after");
    check(!t.has("ab") && t.has("abcdef") && t.has("abcxyz"), "only 'ab' went");

    for (const k of ["abcdef", "abcxyz"]) t.delete(k);
    eq(t.size, 0, "emptied");
    eq(t.keysWithPrefix("").length, 0, "and enumerates nothing");
    t.insert("fresh");
    eq(t.size, 1, "an emptied trie still accepts a key");
    check(t.has("fresh"), "and finds it");
}

{
    const shapes = [
        ["long shared paths", (i) => "/api/v2/users/" + i + "/profile/settings"],
        ["short keys",        (i) => i.toString(36)],
        ["one long chain",    (i) => "z".repeat(i % 40) + i],
        ["byte range",        (i) => String.fromCharCode(32 + (i % 95)) + "/" + i],
    ];
    for (const [label, gen] of shapes) {
        const keys = [];
        for (let i = 0; i < 3000; i++) keys.push(gen(i));
        const probes = [];
        for (let i = 0; i < 500; i++) {
            const k = gen(i);
            probes.push(k.slice(0, Math.max(0, k.length - 1)));
            probes.push(k + "!");
            probes.push(gen(i + 100000));
        }
        const t = agree(keys, probes, label);
        let bad = null;
        for (let i = 0; i < 200; i++) { const k = gen(i);
            if (t.longestPrefix(k) !== k) { bad = k; break; } }
        check(bad === null, label + ": longestPrefix of a stored key is itself, failed at " +
              JSON.stringify(bad));
    }
}

{
    const keys = [];
    for (let i = 0; i < 2000; i++) keys.push("user/" + (i % 13) + "/item/" + i.toString(36));
    const a = new Trie(); for (const k of keys) a.insert(k);
    const b = new Trie(); for (const k of keys.slice().reverse()) b.insert(k);
    const ra = a.serialize(), rb = b.serialize();
    let same = ra.length === rb.length;
    if (same) for (let i = 0; i < ra.length; i++) if (ra[i] !== rb[i]) { same = false; break; }
    check(same, "two insert orders give one record even with tails");

    const back = Trie.deserialize(ra);
    eq(back.size, a.size, "decoded size");
    let bad = null;
    for (const k of keys) if (!back.has(k)) { bad = k; break; }
    check(bad === null, "every key survives, lost " + JSON.stringify(bad));
    const rc = back.serialize();
    same = rc.length === ra.length;
    if (same) for (let i = 0; i < ra.length; i++) if (ra[i] !== rc[i]) { same = false; break; }
    check(same, "re-encoding a decoded trie is byte-identical");
}

if (fails === 0) print("test_structures_trie_paths: all " + n + " checks passed");
else print("test_structures_trie_paths: " + fails + " FAILED of " + n);
