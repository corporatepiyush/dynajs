// 36 · Search with typo tolerance — autocomplete, "did you mean", and keyword tagging over a product catalogue.
//
// WHAT IT SHOWS
//   - dyna:structures Trie: prefix completion as the user types
//   - dyna:matcher DamerauLevenshtein + JaroWinkler: ranking candidates for a misspelt query
//   - a length filter and a distance cap, so fuzzy matching never scans the whole catalogue slowly
//   - dyna:matcher MultiMatcher: tagging text with hundreds of keywords in one pass
//
// RUN      dynajs examples/apps/36-fuzzy-search-suggestions.js

import { Trie } from "dyna:structures";
import { DamerauLevenshtein, JaroWinkler, MultiMatcher } from "dyna:matcher";

const catalogue = [
    "mechanical keyboard", "membrane keyboard", "wireless mouse", "gaming mouse", "mouse pad",
    "usb-c cable", "usb-c hub", "hdmi cable", "monitor arm", "monitor stand", "webcam",
    "headphones", "headset", "microphone", "laptop stand", "laptop sleeve", "power bank",
];

// ---- index -----------------------------------------------------------------
// Completion works on whole product names and on each word inside them, so
// typing "key" finds "mechanical keyboard" too.
const trie = new Trie();
const byWord = new Map();                       // word -> products containing it
for (const name of catalogue) {
    trie.insert(name);
    for (const word of name.split(" ")) {
        trie.insert(word);
        byWord.set(word, [...(byWord.get(word) ?? []), name]);
    }
}
const vocabulary = [...byWord.keys()];

function complete(prefix, limit = 5) {
    const hits = new Set();
    for (const key of trie.keysWithPrefix(prefix.toLowerCase())) {
        if (catalogue.includes(key)) hits.add(key);
        for (const product of byWord.get(key) ?? []) hits.add(product);
    }
    return [...hits].sort().slice(0, limit);
}

// ---- did you mean ----------------------------------------------------------
// Correct each word independently against the vocabulary.
function correctWord(word) {
    if (byWord.has(word)) return word;
    const maxEdits = word.length <= 4 ? 1 : 2;   // short words tolerate fewer edits
    let best = null;
    for (const candidate of vocabulary) {
        // Cheap rejection first: words whose lengths differ by more than the
        // budget cannot be within it.
        if (Math.abs(candidate.length - word.length) > maxEdits) continue;
        // {max} stops the computation as soon as the budget is exceeded.
        const edits = DamerauLevenshtein(word, candidate, { max: maxEdits });
        if (edits > maxEdits) continue;
        // Ties on edit distance are broken by Jaro-Winkler, which rewards a
        // shared prefix: people usually get the first letters right.
        const score = edits - JaroWinkler(word, candidate);
        if (!best || score < best.score) best = { candidate, score };
    }
    return best?.candidate ?? null;
}

function search(query) {
    const words = query.toLowerCase().trim().split(/\s+/);
    const corrected = words.map((w) => correctWord(w) ?? w);
    // A product matches when it contains every (corrected) query word.
    const results = catalogue.filter((name) => corrected.every((w) => name.split(" ").includes(w)));
    const changed = corrected.join(" ") !== words.join(" ");
    return { results, didYouMean: changed && results.length ? corrected.join(" ") : null };
}

// ---- keyword tagging -------------------------------------------------------
// One automaton finds every catalogue term in a support ticket in a single
// scan, however many terms there are.
const tagger = new MultiMatcher(vocabulary);
function tags(text) {
    const found = new Set();
    for (const hit of tagger.allIn(text.toLowerCase())) found.add(vocabulary[hit.index]);
    return [...found].sort();
}

// ---- demo ------------------------------------------------------------------
console.log('complete("mo"):', complete("mo").join(", "));
for (const q of ["mechancial keybaord", "wirless mose", "hdmi cabel", "quantum flux"]) {
    const r = search(q);
    console.log(`"${q}" ->`, r.results.join(", ") || "(nothing)", r.didYouMean ? `[did you mean "${r.didYouMean}"?]` : "");
}

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
check(complete("key").join() === "mechanical keyboard,membrane keyboard", "a word inside a name completes");
check(complete("usb").length === 2 && complete("zzz").length === 0, "prefixes select and miss correctly");
const typo = search("mechancial keybaord");
check(typo.results.join() === "mechanical keyboard" && typo.didYouMean === "mechanical keyboard", "transposed letters are corrected");
check(search("wirless mose").results.join() === "wireless mouse", "missing letters are corrected");
check(search("monitor").results.length === 2 && search("monitor").didYouMean === null, "a correct query gets no suggestion");
check(search("quantum flux").results.length === 0, "nonsense finds nothing rather than something random");
check(correctWord("cabel") === "cable", "a short word tolerates one edit");
check(correctWord("mouth") === "mouse" || correctWord("mouth") === null, "near-words resolve to a real term or nothing");
check(tags("My headset mic died; the USB-C cable for the webcam is fine").join() === "cable,headset,usb-c,webcam",
      "terms are tagged in one pass");
console.log("self-test passed");
