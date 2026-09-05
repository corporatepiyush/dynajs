/*
 * Trie path compression (lazy expansion).
 *
 * A childless node stores the rest of its single key inline as a `tail`, so
 * most of a real key set costs one node per BRANCH rather than one per
 * character. Every read path then has two shapes to handle, and the second is
 * the one that gets forgotten:
 *
 *   has / delete       a key can end AT a node or INSIDE a tail
 *   keysWithPrefix     a prefix can end inside a tail, and the single key
 *                      below still starts with it -- answering "no key has
 *                      this prefix" there is wrong and silent
 *   longestPrefix      a tail can complete a longer stored key
 *   serialize          the walk visits nodes, so a tail's key is never reached
 *                      as a node and has to be assembled
 *
 * Insertion splits a tail whenever a new key diverges inside it, and the split
 * has four shapes: diverge at the start, in the middle, at the last byte, and
 * "the new key stops exactly where the tail begins". Each is covered by name.
 *
 * The oracle throughout is a JS Set of the same keys: every membership query
 * is checked against it, and every enumeration is compared as a sorted list.
 */
import { Trie } from "dyna:structures";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }

/* Build a trie and a Set from the same keys, then assert they agree about
   membership for every key AND for a set of near-misses that share prefixes. */
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
    /* keysWithPrefix("") must enumerate exactly the set */
    const all = t.keysWithPrefix("").sort();
    const want = [...s].sort();
    let same = all.length === want.length;
    if (same) for (let i = 0; i < all.length; i++) if (all[i] !== want[i]) { same = false; break; }
    check(same, label + ": enumeration differs -- got " + JSON.stringify(all.slice(0, 6)) +
          " want " + JSON.stringify(want.slice(0, 6)));
    return t;
}

/* ------------------------------------------- 1. every split shape by name */
{
    /* the base case: one key, wholly a tail on the root */
    agree(["abcdef"], ["a", "ab", "abcde", "abcdefg", "", "x"], "a single key");

    /* diverge at the FIRST byte of the tail */
    agree(["abcdef", "axxxxx"], ["a", "ab", "ax", "abcdef", "axxxxx"], "diverge at byte 0");
    /* diverge in the MIDDLE */
    agree(["abcdef", "abcxyz"], ["abc", "abcd", "abcx", "ab"], "diverge mid-tail");
    /* diverge at the LAST byte */
    agree(["abcdef", "abcdex"], ["abcde", "abcdef", "abcdex"], "diverge at the last byte");
    /* the new key STOPS where the tail still has bytes left */
    agree(["abcdef", "abc"], ["abc", "abcd", "abcdef"], "new key stops inside the tail");
    /* the new key EXTENDS past the tail */
    agree(["abc", "abcdef"], ["abc", "abcd", "abcdef", "abcdefg"], "new key extends the tail");
    /* the new key IS the tail key */
    agree(["abcdef", "abcdef"], ["abcdef"], "re-inserting the same key");
    /* a key that is a strict prefix, inserted first and second */
    agree(["ab", "abcd", "abcdef"], ["a", "ab", "abc", "abcd", "abcde", "abcdef"],
          "a chain of prefixes");
    agree(["abcdef", "abcd", "ab"], ["a", "ab", "abc", "abcd", "abcde", "abcdef"],
          "the same chain, longest first");

    /* the empty key alongside a tail */
    agree(["", "abcdef"], ["", "a", "abcdef"], "the empty key and a tail");
    agree(["abcdef", ""], ["", "abcdef"], "a tail then the empty key");
}

/* --------------------------------- 2. keysWithPrefix ENDING inside a tail
   This is the case that answers silently wrong: the prefix is not a whole key
   and not a node, but the one key below the tail does start with it. */
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

    /* the same, with branches so the prefix walk crosses nodes AND a tail */
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

/* ---------------------------------------- 3. longestPrefix through a tail */
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

/* ------------------------------------------- 4. delete, of a tail and a node */
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
    /* re-inserting must work, and must not double-count */
    t.insert("abcdef");
    eq(t.size, 3, "re-inserting restores the count");
    check(t.has("abcdef"), "and the key");

    /* delete the node key, leaving the tails */
    check(t.delete("ab"), "deleting a node key");
    eq(t.size, 2, "size after");
    check(!t.has("ab") && t.has("abcdef") && t.has("abcxyz"), "only 'ab' went");

    /* delete everything, then reuse */
    for (const k of ["abcdef", "abcxyz"]) t.delete(k);
    eq(t.size, 0, "emptied");
    eq(t.keysWithPrefix("").length, 0, "and enumerates nothing");
    t.insert("fresh");
    eq(t.size, 1, "an emptied trie still accepts a key");
    check(t.has("fresh"), "and finds it");
}

/* --------------------------- 5. a differential over many shapes at once */
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
            probes.push(k.slice(0, Math.max(0, k.length - 1)));   /* near miss */
            probes.push(k + "!");                                  /* extension */
            probes.push(gen(i + 100000));                          /* absent */
        }
        const t = agree(keys, probes, label);
        /* every stored key must also be its own longest prefix */
        let bad = null;
        for (let i = 0; i < 200; i++) { const k = gen(i);
            if (t.longestPrefix(k) !== k) { bad = k; break; } }
        check(bad === null, label + ": longestPrefix of a stored key is itself, failed at " +
              JSON.stringify(bad));
    }
}

/* ------------------------------ 6. the record is unchanged by compression
   Path compression is an internal representation. The record is a function of
   the key SET, so a compressed trie and one built key-by-key in a different
   order must serialize to the same bytes and decode to the same keys. */
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
