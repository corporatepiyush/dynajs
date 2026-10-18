// Translation memory — fuzzy segment matching for a CAT tool: candidate retrieval from an inverted index, edit-distance
// scoring, placeholder transfer, terminology QA and a leverage report, with retrieval checked against brute force.
//
// WHAT IT IS
//   What a translator's tool does when a new document arrives:
//
//   memory      20 000 English -> German segment pairs; numbers and inline tags are normalised to placeholders before
//               indexing, so "Order 4411 ships in 3 days" and "Order 87 ships in 5 days" are the same segment
//   retrieve    BM25 over the normalised source text (dyna:structures InvertedIndex, OR mode) returns 30 candidates;
//               only those are scored, instead of the whole memory
//   score       fuzzy match = 1 - Levenshtein / max length on normalised text (dyna:matcher, with a max bound so
//               hopeless candidates stop early); 100% matches are exact, 75% is the usual floor
//   transfer    the matched target gets the NEW segment's numbers and tags put back in placeholder order
//   explain     DiffWords between the new source and the matched source shows the translator what changed
//   QA          a glossary (MultiMatcher over source terms) requires each matched term's approved translation in the
//               target; a missing term is flagged
//   oracle      for 300 queries the best match over ALL 20 000 entries is found by brute force; the index-backed best
//               must equal it, and the leverage buckets are tallied the way CAT tools report them
//
// MODULES  dyna:structures (InvertedIndex, Multiset) · dyna:matcher (Levenshtein, DiffWords, MultiMatcher) ·
//          dyna:random · dyna:cli · String/Array extensions
//
// RUN      dynajs examples/text/translation-memory.js

import { InvertedIndex, Multiset } from "dyna:structures";
import { Levenshtein, DiffWords, MultiMatcher } from "dyna:matcher";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(1954);

// =============================================================================
// Synthetic memory: templates x vocabulary, aligned EN/DE
// =============================================================================
const NOUNS = [
    ["printer", "Drucker"],
    ["router", "Router"],
    ["laptop", "Laptop"],
    ["invoice", "Rechnung"],
    ["account", "Konto"],
    ["password", "Passwort"],
    ["battery", "Akku"],
    ["screen", "Bildschirm"],
    ["update", "Update"],
    ["order", "Bestellung"],
    ["warranty", "Garantie"],
    ["cable", "Kabel"],
];
const TEMPLATES = [
    ["Restart the {a} before you connect the {b}.", "Starten Sie den {a} neu, bevor Sie den {b} anschließen."],
    ["Your {a} will be delivered in {n} days.", "Ihr {a} wird in {n} Tagen geliefert."],
    ["Press <b>OK</b> to confirm the {a}.", "Drücken Sie <b>OK</b>, um den {a} zu bestätigen."],
    ["The {a} could not be found on the {b}.", "Der {a} wurde auf dem {b} nicht gefunden."],
    ["Order {n} contains {n} items and one {a}.", "Bestellung {n} enthält {n} Artikel und einen {a}."],
    ["Check the {a} settings and try again.", "Überprüfen Sie die {a}-Einstellungen und versuchen Sie es erneut."],
    [
        "If the {a} is damaged, contact support within {n} days.",
        "Wenn der {a} beschädigt ist, wenden Sie sich innerhalb von {n} Tagen an den Support.",
    ],
];
const ADVERBS = ["", "carefully ", "quickly ", "immediately ", "always "];
function make(t, a, b) {
    let [en, de] = TEMPLATES[t];
    const nums = [];
    const fill = (s, lang) => s.replace(/\{a\}/g, NOUNS[a][lang]).replace(/\{b\}/g, NOUNS[b][lang]);
    en = fill(en, 0);
    de = fill(de, 1);
    en = en.replace(/\{n\}/g, () => {
        const n = String(1 + rng.nextBounded(500));
        nums.push(n);
        return n;
    });
    let k = 0;
    de = de.replace(/\{n\}/g, () => nums[k++]);
    return [en, de];
}
const memory = [];
for (let i = 0; i < 20_000; i++) {
    const t = rng.nextBounded(TEMPLATES.length),
        a = rng.nextBounded(NOUNS.length),
        b = rng.nextBounded(NOUNS.length);
    let [en, de] = make(t, a, b);
    const adv = ADVERBS[rng.nextBounded(ADVERBS.length)];
    if (adv && en.startsWith("Restart")) en = en.replace("Restart ", `${adv[0].toUpperCase()}${adv.slice(1)}restart `);
    memory.push({ en, de });
}

// =============================================================================
// Normalisation and index
// =============================================================================
const PLACEHOLDER = /<[^>]+>|\d+/g;
const normalise = (s) => s.replace(PLACEHOLDER, (m) => (m.startsWith("<") ? "{t}" : "{n}"));
const placeholders = (s) => s.match(PLACEHOLDER) ?? [];
const index = new InvertedIndex();
const seen = new Map();
memory.forEach((m, i) => {
    m.norm = normalise(m.en);
    if (!seen.has(m.norm)) {
        seen.set(m.norm, i);
        index.add(i, m.norm.replace(/\{[nt]\}/g, " "));
    }
});
const uniqueSources = seen.size;

const fuzzy = (a, b) => {
    const max = Math.max(a.length, b.length);
    const d = Levenshtein(a, b, { max: Math.ceil(max * 0.4) });
    return 1 - Math.min(d, max) / max;
};
function lookup(segment, k = 30) {
    const norm = normalise(segment);
    const cands = index.search(norm.replace(/\{[nt]\}/g, " "), { mode: "or", limit: k });
    let best = null;
    for (const [id] of cands) {
        const s = fuzzy(norm, memory[id].norm);
        if (!best || s > best.score || (s === best.score && id < best.id)) best = { id, score: s };
    }
    if (!best) return null;
    const vals = placeholders(segment);
    let k2 = 0;
    const target = memory[best.id].de.replace(PLACEHOLDER, (m) =>
        m.startsWith("<") ? m : (vals.filter((v) => !v.startsWith("<"))[k2++] ?? m),
    );
    return { ...best, norm, target, source: memory[best.id].en };
}
function bruteBest(segment) {
    const norm = normalise(segment);
    let best = null;
    for (const [n, id] of seen) {
        const s = fuzzy(norm, n);
        if (!best || s > best.score || (s === best.score && id < best.id)) best = { id, score: s };
    }
    return best;
}

// =============================================================================
// Checks
// =============================================================================
{
    const r = lookup("Your laptop will be delivered in 12 days.");
    results["a known segment with new numbers is a 100% match"] = r.score === 1;
    results["the new number is transferred into the target"] = r.target === "Ihr Laptop wird in 12 Tagen geliefert.";
    const r2 = lookup("Order 77 contains 3 items and one cable.");
    results["placeholders transfer in order"] = r2.target === "Bestellung 77 enthält 3 Artikel und einen Kabel.";
    const tag = lookup("Press <i>OK</i> to confirm the order.");
    results["inline tags normalise to the same placeholder"] = tag.score === 1;
}
{
    const fresh = "Restart the printer gently before you connect the router.";
    const r = lookup(fresh);
    const diff = DiffWords(r.source, fresh)
        .filter((h) => h.op !== 0)
        .map((h) => `${h.op > 0 ? "+" : "-"}${h.text.trim()}`)
        .filter((x) => x.length > 1);
    results["a near segment is a fuzzy match above 85%"] = r.score > 0.85 && r.score < 1;
    results["DiffWords shows only the inserted word"] = diff.length === 1 && diff[0] === "+gently";
}
{
    let agree = 0,
        scoreSum = 0;
    const N = 300,
        buckets = new Multiset();
    for (let i = 0; i < N; i++) {
        const t = rng.nextBounded(TEMPLATES.length),
            a = rng.nextBounded(NOUNS.length),
            b = rng.nextBounded(NOUNS.length);
        let [q] = make(t, a, b);
        if (i % 3 === 1) q = q.replace(/\.$/, " now.");
        if (i % 3 === 2) q = q.split(" ").reverse().slice(0, 4).join(" ") + " " + q;
        const fast = lookup(q),
            slow = bruteBest(q);
        if (fast && Math.abs(fast.score - slow.score) < 1e-12) agree++;
        scoreSum += fast?.score ?? 0;
        const s = fast?.score ?? 0;
        buckets.add(s === 1 ? "100%" : s >= 0.95 ? "95-99%" : s >= 0.85 ? "85-94%" : s >= 0.75 ? "75-84%" : "no match");
    }
    results["index-backed best match equals the brute-force best for 300 queries"] = agree === N;
    print(
        Table(
            ["100%", "95-99%", "85-94%", "75-84%", "no match"].map((b) => [
                b,
                buckets.count(b),
                ((100 * buckets.count(b)) / N).toFixed(1) + "%",
            ]),
            { head: ["leverage", "segments", "share"] },
        ),
    );
    results["leverage report covers every segment"] = buckets.entrySet().reduce((s, [, n]) => s + n, 0) === N;
}
{
    const glossary = [
        ["password", "Passwort"],
        ["warranty", "Garantie"],
        ["invoice", "Rechnung"],
    ];
    const terms = new MultiMatcher(glossary.map(([en]) => en));
    const qa = (src, tgt) =>
        terms
            .allIn(src.toLowerCase())
            .map((m) => glossary[m.index])
            .filter(([, de]) => !tgt.includes(de))
            .map(([en]) => en);
    const ok = lookup("Check the password settings and try again.");
    results["approved terminology passes QA"] =
        qa("Check the password settings and try again.", ok.target).length === 0;
    results["a target missing the approved term is flagged"] =
        qa("Check the password settings and try again.", "Überprüfen Sie die Kennwort-Einstellungen.").join() ===
        "password";
}
{
    const t0 = performance.now();
    for (let i = 0; i < 200; i++) lookup(memory[i * 97].en.replace(/\d+/, "9"));
    const fastMs = (performance.now() - t0) / 200;
    const t1 = performance.now();
    for (let i = 0; i < 20; i++) bruteBest(memory[i * 97].en);
    const bruteMs = (performance.now() - t1) / 20;
    results["index-backed lookup is faster than scanning the memory"] = fastMs < bruteMs;
    print(
        `memory: ${memory.length.format()} pairs, ${uniqueSources.format()} unique normalised sources; lookup ${fastMs.toFixed(2)} ms vs brute force ${bruteMs.toFixed(2)} ms`,
    );
}

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
