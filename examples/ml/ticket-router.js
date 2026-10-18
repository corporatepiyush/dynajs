// Ticket router — language detection, PII redaction, intent classification, duplicate merging, KB deflection and
// skill-based assignment for a multilingual support inbox.
//
// WHAT IT IS
//   The front door of a support organisation that receives tickets in English, German and Spanish:
//
//   language     a character-trigram logistic model (sparse CSR features) picks the language, so each ticket
//                goes to a team that can read it
//   redaction    card numbers (Luhn-checked), IBANs (mod-97-checked), emails and phone numbers are replaced with
//                typed placeholders BEFORE anything is stored or classified
//   intent       word and bigram TF-IDF -> logistic regression (one-vs-rest over 6 intents), trained on a labelled
//                history; tickets under 60% confidence go to a human triage queue (active learning)
//   duplicates   MinHash signatures with banding (LSH) find near-duplicate tickets from the same incident and
//                merge them into a parent, so one outage is one ticket with 40 followers
//   deflection   a BM25 search over the knowledge base; a confident article match is sent as an auto-reply
//                rendered from a template in the customer's language
//   assignment   agents have languages, skills and capacity; a priority heap assigns the oldest highest-priority
//                ticket to the least-loaded qualified agent
//   API          POST /tickets returns the routing decision; GET /queues shows the state
//   trends       word bigrams of the historical inbox are counted in a CountMinSketch; today's bigrams go to a
//                TopK, and the ones far above their historical rate are surfaced as a spike ("error 503" during
//                an outage) before anyone has read a ticket
//   KB search    the knowledge base is an InvertedIndex (BM25 in C), queried per ticket
//
// MODULES  dyna:ml (LogisticRegression, CSR, metrics) · dyna:http · dyna:structures (Heap, Multimap, InvertedIndex,
//          CountMinSketch, TopK)
//          · dyna:hash (Murmur3) · dyna:html (Template) · dyna:matcher (MultiMatcher) · dyna:random
//
// RUN      dynajs examples/ml/ticket-router.js     trains on 3,000 historical tickets and routes 400 new ones

import { LogisticRegression, CSR, accuracy, f1, confusionMatrix } from "dyna:ml";
import { App } from "dyna:http";
import { Heap, Multimap, InvertedIndex, CountMinSketch, TopK } from "dyna:structures";
import { Murmur3_128Hex, XXHash32 } from "dyna:hash";
import { Template } from "dyna:html";
import { MultiMatcher } from "dyna:matcher";
import { Random } from "dyna:random";

const rng = new Random(77);

// =============================================================================
// Synthetic inbox in three languages
// =============================================================================
const INTENTS = ["billing", "login", "outage", "shipping", "refund", "feature"];
const PHRASES = {
    en: {
        billing: [
            "I was charged twice on my invoice",
            "the invoice amount is wrong",
            "why did my subscription price change",
            "please update my billing address",
        ],
        login: [
            "I cannot log in to my account",
            "password reset email never arrives",
            "two factor code is rejected",
            "my account is locked",
        ],
        outage: [
            "the dashboard is down",
            "getting error 503 on every page",
            "the app is not loading at all",
            "API requests time out since this morning",
        ],
        shipping: [
            "where is my package",
            "tracking number shows no updates",
            "the delivery is late",
            "order arrived damaged in shipping",
        ],
        refund: [
            "I want my money back",
            "please refund my last order",
            "refund has not arrived yet",
            "cancel and refund the subscription",
        ],
        feature: [
            "it would be great to export to excel",
            "please add dark mode",
            "can you support single sign on",
            "feature request for bulk edit",
        ],
    },
    de: {
        billing: [
            "mir wurde die Rechnung doppelt berechnet",
            "der Rechnungsbetrag ist falsch",
            "warum hat sich der Preis geändert",
            "bitte Rechnungsadresse ändern",
        ],
        login: [
            "ich kann mich nicht anmelden",
            "die E-Mail zum Zurücksetzen des Passworts kommt nicht",
            "der Bestätigungscode wird abgelehnt",
            "mein Konto ist gesperrt",
        ],
        outage: [
            "das Dashboard ist nicht erreichbar",
            "Fehler 503 auf jeder Seite",
            "die App lädt überhaupt nicht",
            "API Anfragen laufen seit heute Morgen ab",
        ],
        shipping: [
            "wo ist mein Paket",
            "die Sendungsverfolgung zeigt nichts",
            "die Lieferung ist verspätet",
            "die Bestellung kam beschädigt an",
        ],
        refund: [
            "ich möchte mein Geld zurück",
            "bitte erstatten Sie meine letzte Bestellung",
            "die Rückerstattung ist noch nicht da",
            "Abo kündigen und erstatten",
        ],
        feature: [
            "Export nach Excel wäre toll",
            "bitte einen dunklen Modus hinzufügen",
            "unterstützt ihr Single Sign On",
            "Wunsch nach Massenbearbeitung",
        ],
    },
    es: {
        billing: [
            "me cobraron dos veces en la factura",
            "el importe de la factura es incorrecto",
            "por qué cambió el precio de mi suscripción",
            "actualicen mi dirección de facturación",
        ],
        login: [
            "no puedo iniciar sesión en mi cuenta",
            "nunca llega el correo para restablecer la contraseña",
            "el código de verificación es rechazado",
            "mi cuenta está bloqueada",
        ],
        outage: [
            "el panel no funciona",
            "error 503 en todas las páginas",
            "la aplicación no carga",
            "las peticiones a la API fallan desde esta mañana",
        ],
        shipping: [
            "dónde está mi paquete",
            "el número de seguimiento no se actualiza",
            "la entrega llega tarde",
            "el pedido llegó dañado",
        ],
        refund: [
            "quiero que me devuelvan el dinero",
            "reembolsen mi último pedido",
            "el reembolso todavía no ha llegado",
            "cancelar y reembolsar la suscripción",
        ],
        feature: [
            "sería genial exportar a excel",
            "agreguen modo oscuro por favor",
            "pueden soportar inicio de sesión único",
            "solicitud de edición masiva",
        ],
    },
};
const FILLER = {
    en: ["hello", "thanks", "asap please", "this is urgent", "regards"],
    de: ["hallo", "danke", "bitte schnell", "das ist dringend", "viele Grüße"],
    es: ["hola", "gracias", "por favor rápido", "es urgente", "saludos"],
};
const LANGS = Object.keys(PHRASES);
const luhnCard = () => {
    const d = Array.from({ length: 15 }, () => rng.nextBounded(10));
    d[0] = 4;
    const s = d.reduceRight((a, x, i) => {
        let v = x;
        if ((15 - i) % 2 === 1) {
            v *= 2;
            if (v > 9) v -= 9;
        }
        return a + v;
    }, 0);
    return [...d, (10 - (s % 10)) % 10].join("");
};
function makeTicket(i, intent = INTENTS[rng.nextBounded(INTENTS.length)], lang = LANGS[rng.nextBounded(3)]) {
    const p = PHRASES[lang][intent],
        f = FILLER[lang];
    let text = `${f[rng.nextBounded(f.length)]}, ${p[rng.nextBounded(p.length)]}. ${rng.nextFloat() < 0.5 ? p[rng.nextBounded(p.length)] + ". " : ""}${f[rng.nextBounded(f.length)]}`;
    if (intent === "billing" && rng.nextFloat() < 0.3)
        text += ` card ${luhnCard()
            .replace(/(\d{4})/g, "$1 ")
            .trim()}`;
    if (intent === "refund" && rng.nextFloat() < 0.3) text += ` IBAN DE89 3704 0044 0532 0130 00`;
    if (rng.nextFloat() < 0.3)
        text += ` contact me at user${i}@mail.example or +49 151 ${String(1000000 + rng.nextBounded(8999999))}`;
    return { id: `T${i}`, intent, lang, text, at: i };
}
const history = Array.from({ length: 3000 }, (_, i) => makeTicket(i));
const incoming = Array.from({ length: 400 }, (_, i) => makeTicket(10000 + i));
const intentOf = new Map();
const outageText = "API requests time out since this morning, getting error 503 on every page";
for (let k = 0; k < 40; k++)
    incoming.push({
        id: `T2${k}`,
        intent: "outage",
        lang: "en",
        at: 10400 + k,
        text: `${FILLER.en[k % 5]}, ${outageText}${k % 3 ? " for our team" : ""}${k % 4 ? ". customer " + k : ""}`,
    });

// =============================================================================
// Redaction
// =============================================================================
const luhnOk = (d) =>
    d
        .split("")
        .reverse()
        .reduce((s, c, i) => {
            let v = Number(c);
            if (i % 2) {
                v *= 2;
                if (v > 9) v -= 9;
            }
            return s + v;
        }, 0) %
        10 ===
    0;
const ibanOk = (s) => {
    const r = (s.slice(4) + s.slice(0, 4)).replace(/[A-Z]/g, (c) => String(c.charCodeAt(0) - 55));
    let m = 0;
    for (const ch of r) m = (m * 10 + Number(ch)) % 97;
    return m === 1;
};
function redact(text) {
    const found = [];
    let out = text.replace(/\b\d(?:[ -]?\d){12,18}\b/g, (m) => {
        const d = m.replace(/\D/g, "");
        if (luhnOk(d)) {
            found.push("card");
            return "[CARD]";
        }
        return m;
    });
    out = out.replace(/\b[A-Z]{2}\d{2}(?: ?[A-Z0-9]{4}){3,7}(?: ?[A-Z0-9]{1,3})?\b/g, (m) => {
        const s = m.replace(/ /g, "");
        if (ibanOk(s)) {
            found.push("iban");
            return "[IBAN]";
        }
        return m;
    });
    out = out.replace(/[\w.+-]+@[\w-]+\.[\w.]+/g, () => {
        found.push("email");
        return "[EMAIL]";
    });
    out = out.replace(/\+\d{1,3}(?: ?\d{2,4}){2,4}/g, () => {
        found.push("phone");
        return "[PHONE]";
    });
    return { text: out, found };
}

// =============================================================================
// Features: hashed trigrams (language) and hashed word/bigram TF-IDF (intent), as CSR
// =============================================================================
const DIM = 1 << 14;
const tokens = (t) =>
    t
        .toLowerCase()
        .normalize("NFC")
        .replace(/\[\w+\]/g, " ")
        .match(/\p{L}+|\d+/gu) || [];
function toCSR(rows, featurize, idf) {
    const values = [],
        columns = [],
        ptr = [0];
    for (const r of rows) {
        const counts = new Map();
        for (const f of featurize(r)) {
            const h = XXHash32(f, 1) % DIM;
            counts.set(h, (counts.get(h) || 0) + 1);
        }
        const entries = [...counts].sort((a, b) => a[0] - b[0]);
        let norm = 0;
        const ws = entries.map(([h, c]) => {
            const w = (1 + Math.log(c)) * (idf ? idf[h] || 1 : 1);
            norm += w * w;
            return w;
        });
        norm = Math.sqrt(norm) || 1;
        entries.forEach(([h], k) => {
            columns.push(h);
            values.push(ws[k] / norm);
        });
        ptr.push(values.length);
    }
    return new CSR(Float64Array.from(values), columns, ptr, DIM);
}
const trigrams = (t) => {
    const s = ` ${t.toLowerCase()} `,
        out = [];
    for (let i = 0; i + 3 <= s.length; i++) out.push(s.slice(i, i + 3));
    return out;
};
const words = (t) => {
    const w = tokens(t);
    return [...w, ...w.slice(1).map((x, i) => `${w[i]}_${x}`)];
};
const df = new Map();
for (const t of history)
    for (const f of new Set(words(redact(t.text).text))) {
        const h = XXHash32(f, 1) % DIM;
        df.set(h, (df.get(h) || 0) + 1);
    }
const idf = {};
for (const [h, n] of df) idf[h] = Math.log((history.length + 1) / (n + 1)) + 1;

const split = 2400,
    train = history.slice(0, split),
    test = history.slice(split);
const langModel = await new LogisticRegression({ maxIter: 600, learningRate: 2 }).fit(
    toCSR(
        train.map((t) => t.text),
        trigrams,
    ),
    train.map((t) => LANGS.indexOf(t.lang)),
);
const intentModel = await new LogisticRegression({ maxIter: 1500, learningRate: 3 }).fit(
    toCSR(
        train.map((t) => redact(t.text).text),
        words,
        idf,
    ),
    train.map((t) => INTENTS.indexOf(t.intent)),
);
const langPred = await langModel.predict(
    toCSR(
        test.map((t) => t.text),
        trigrams,
    ),
);
const intentPred = await intentModel.predict(
    toCSR(
        test.map((t) => redact(t.text).text),
        words,
        idf,
    ),
);
const evalScores = {
    lang: await accuracy(
        test.map((t) => LANGS.indexOf(t.lang)),
        langPred,
    ),
    intent: await accuracy(
        test.map((t) => INTENTS.indexOf(t.intent)),
        intentPred,
    ),
    outageF1: await f1(
        test.map((t) => (t.intent === "outage" ? 1 : 0)),
        intentPred.map((p) => (p === INTENTS.indexOf("outage") ? 1 : 0)),
    ),
};
const confusion = await confusionMatrix(
    test.map((t) => INTENTS.indexOf(t.intent)),
    intentPred,
);

// =============================================================================
// Near-duplicates: MinHash + LSH banding
// =============================================================================
const HASHES = 64,
    BANDS = 16,
    ROWS = HASHES / BANDS;
const shingles = (t) => {
    const w = tokens(t);
    const s = new Set();
    for (let i = 0; i + 3 <= w.length; i++) s.add(w.slice(i, i + 3).join(" "));
    return s;
};
const minhash = (set) =>
    Array.from({ length: HASHES }, (_, k) => {
        let m = 0xffffffff;
        for (const s of set) m = Math.min(m, XXHash32(s, k * 7919 + 1));
        return m;
    });
const lsh = new Multimap(),
    sigs = new Map(),
    parentOf = new Map();
function findDuplicate(id, text) {
    const sig = minhash(shingles(text));
    const candidates = new Set();
    for (let b = 0; b < BANDS; b++) {
        const key = `${b}:${Murmur3_128Hex(sig.slice(b * ROWS, (b + 1) * ROWS).join(","))}`;
        for (const other of lsh.get(key)) candidates.add(other);
        lsh.put(key, id);
    }
    sigs.set(id, sig);
    let best = null;
    for (const c of candidates) {
        const s = sigs.get(c),
            j = s.filter((v, k) => v === sig[k]).length / HASHES;
        if (j >= 0.6 && (!best || j > best[1])) best = [parentOf.get(c) || c, j];
    }
    return best;
}

// =============================================================================
// Knowledge base deflection
// =============================================================================
const KB = [
    {
        id: "KB-1",
        intent: "login",
        title: { en: "Reset your password", de: "Passwort zurücksetzen", es: "Restablecer la contraseña" },
        body: "password reset email spam folder link expires account locked two factor code passwort zurücksetzen konto gesperrt bestätigungscode contraseña restablecer cuenta bloqueada código",
    },
    {
        id: "KB-2",
        intent: "shipping",
        title: { en: "Track your order", de: "Sendung verfolgen", es: "Seguir tu pedido" },
        body: "tracking number package delivery late carrier updates paket sendungsverfolgung lieferung verspätet paquete seguimiento entrega tarde",
    },
    {
        id: "KB-3",
        intent: "refund",
        title: { en: "Refund timelines", de: "Dauer von Erstattungen", es: "Plazos de reembolso" },
        body: "refund money back arrived days bank cancel subscription geld zurück rückerstattung erstatten reembolso dinero devuelvan reembolsar",
    },
    {
        id: "KB-4",
        intent: "billing",
        title: { en: "Understanding your invoice", de: "Ihre Rechnung verstehen", es: "Entender tu factura" },
        body: "invoice charged twice price change billing address subscription rechnung doppelt berechnet rechnungsbetrag factura cobraron importe facturación",
    },
];
const kb = new InvertedIndex({ k1: 1.2, b: 0.75 });
KB.forEach((a, i) => kb.add(i, tokens(a.body).join(" ")));
function bm25(query) {
    const [hit] = kb.search(tokens(query).join(" "), { mode: "or", limit: 1 });
    return hit ? { a: KB[hit[0]], score: hit[1] } : { a: KB[0], score: 0 };
}
const REPLY = {
    en: new Template(
        "Hi,\n\nthis article may already answer your question: {{title}} (https://help.example/{{id}}).\nReply to this email if it does not and a person will pick it up.\n\nTicket {{ticket}}",
    ),
    de: new Template(
        "Hallo,\n\ndieser Artikel beantwortet Ihre Frage vielleicht schon: {{title}} (https://help.example/{{id}}).\nAntworten Sie, falls nicht, und jemand kümmert sich darum.\n\nTicket {{ticket}}",
    ),
    es: new Template(
        "Hola,\n\neste artículo puede responder a tu pregunta: {{title}} (https://help.example/{{id}}).\nResponde si no es así y una persona lo atenderá.\n\nTicket {{ticket}}",
    ),
};

// =============================================================================
// Agents and assignment
// =============================================================================
const AGENTS = [
    { name: "ana", langs: ["es", "en"], skills: ["billing", "refund", "shipping"], cap: 12 },
    { name: "ben", langs: ["en"], skills: ["outage", "login", "feature"], cap: 15 },
    { name: "clara", langs: ["de", "en"], skills: ["billing", "login", "refund"], cap: 12 },
    { name: "dirk", langs: ["de"], skills: ["outage", "shipping", "feature"], cap: 10 },
    { name: "eva", langs: ["es"], skills: ["outage", "login", "feature"], cap: 10 },
    { name: "finn", langs: ["en", "de", "es"], skills: INTENTS, cap: 8 },
].map((a) => ({ ...a, load: 0, assigned: [] }));
const PRIORITY = { outage: 3, login: 2, billing: 2, refund: 1, shipping: 1, feature: 0 };
const URGENT = new MultiMatcher(["urgent", "asap", "dringend", "urgente", "schnell", "rápido"]);
const waiting = new Heap((a, b) => b.priority - a.priority || a.at - b.at);
const triage = [],
    deflected = [],
    merged = [],
    stored = [];

// Trend baseline: bigram frequencies of the whole history, in a fixed-size sketch.
const bigrams = (t) => {
    const w = tokens(t);
    return w.slice(1).map((x, i) => `${w[i]} ${x}`);
};
const baselineBigrams = new CountMinSketch(1 << 14, 4);
for (const t of history) for (const b of bigrams(redact(t.text).text)) baselineBigrams.add(b);
const today = new TopK(20);
let todayBigrams = 0;

const app = new App({ port: 0 });
app.post("/tickets", async (req) => {
    const t = JSON.parse(req.body);
    const { text, found } = redact(t.text);
    for (const b of bigrams(text)) {
        today.add(b);
        todayBigrams++;
    }
    const [langProba] = await langModel.predictProba(toCSR([t.text], trigrams));
    const lang = LANGS[langProba.indexOf(Math.max(...langProba))];
    const [proba] = await intentModel.predictProba(toCSR([text], words, idf));
    const conf = Math.max(...proba),
        intent = INTENTS[proba.indexOf(conf)];
    const dup = findDuplicate(t.id, text);
    if (dup) {
        parentOf.set(t.id, dup[0]);
        merged.push(t.id);
        return { id: t.id, action: "merged", parent: dup[0], similarity: Number(dup[1].toFixed(2)), redacted: found };
    }
    if (conf < 0.6) {
        triage.push({ id: t.id, text, proba });
        return { id: t.id, action: "triage", lang, confidence: Number(conf.toFixed(2)), redacted: found };
    }
    const kb = bm25(text);
    if (kb.score > 3 && kb.a.intent === intent && intent !== "outage") {
        const reply = REPLY[lang].render({ title: kb.a.title[lang], id: kb.a.id, ticket: t.id });
        deflected.push({ id: t.id, article: kb.a.id, reply });
        return { id: t.id, action: "auto-reply", lang, intent, article: kb.a.id, redacted: found };
    }
    const priority = PRIORITY[intent] + (URGENT.test(text.toLowerCase()) ? 1 : 0);
    const item = { id: t.id, lang, intent, priority, at: t.at, text };
    waiting.push(item);
    stored.push(item);
    return { id: t.id, action: "queued", lang, intent, priority, confidence: Number(conf.toFixed(2)), redacted: found };
});
app.get("/queues", () => ({
    waiting: stored.length,
    triage: triage.length,
    agents: AGENTS.map(({ name, load, cap }) => ({ name, load, cap })),
}));
app.start();
const base = `http://127.0.0.1:${app.port}`;
const decisions = [];
for (const t of incoming) intentOf.set(t.id, t.intent);
for (const t of incoming)
    decisions.push({
        truth: t,
        ...(await (await fetch(base + "/tickets", { method: "POST", body: JSON.stringify(t) })).json()),
    });
const queues = await (await fetch(base + "/queues")).json();
app.close();

const historyBigrams = history.reduce((s, t) => s + bigrams(t.text).length, 0);
const spikes = today
    .list()
    .map(([b, n, err]) => ({
        b,
        n: n - err,
        lift: (n - err) / todayBigrams / ((baselineBigrams.count(b) + 1) / historyBigrams),
    }))
    .filter((x) => x.n >= 10 && x.lift > 5)
    .sortBy((x) => -x.lift);
let unassigned = 0;
const order = [];
for (let item = waiting.pop(); item !== undefined && item !== null; item = waiting.pop()) {
    const ok = AGENTS.filter(
        (a) => a.langs.includes(item.lang) && a.skills.includes(item.intent) && a.load < a.cap,
    ).sort((a, b) => a.load / a.cap - b.load / b.cap);
    if (!ok.length) {
        unassigned++;
        continue;
    }
    ok[0].load++;
    ok[0].assigned.push(item);
    order.push(item.priority);
}

// =============================================================================
// Checks and report
// =============================================================================
const results = {};
const r = redact(
    "pay with 4111 1111 1111 1111 or 4111 1111 1111 1112, IBAN DE89 3704 0044 0532 0130 00, mail a.b+x@ex.com, call +49 151 2345678",
);
results[`redaction checks Luhn and mod-97 (${r.text})`] =
    r.text === "pay with [CARD] or 4111 1111 1111 1112, IBAN [IBAN], mail [EMAIL], call [PHONE]";
results["no PII survives into stored queues"] = !/\d{4} \d{4} \d{4} \d{4}|DE89 3704|@mail\.example|\+49 151/.test(
    JSON.stringify([stored, triage, deflected]),
);
results[`language accuracy ${evalScores.lang.toFixed(3)}`] = evalScores.lang > 0.97;
results[`intent accuracy ${evalScores.intent.toFixed(3)}, outage F1 ${evalScores.outageF1.toFixed(3)}`] =
    evalScores.intent > 0.9 && evalScores.outageF1 > 0.9;
const routed = decisions.filter((d) => d.lang);
results["live language routing matches the ticket's language"] =
    routed.filter((d) => d.lang === d.truth.lang).length / routed.length > 0.97;
const outageMerged = decisions.filter((d) => d.id.startsWith("T2") && d.action === "merged").length;
results[`${outageMerged} of 40 outage reports merged into a parent`] = outageMerged >= 30;
results[`every merge joins tickets about the same thing (${merged.length} merged)`] = merged.every(
    (id) => intentOf.get(id) === intentOf.get(parentOf.get(id)),
);
results[`${deflected.length} tickets deflected with a localised article`] =
    deflected.length > 20 && deflected.every((d) => d.reply.includes(d.id));
results["replies are in the customer's language"] =
    deflected.some((d) => d.reply.startsWith("Hallo")) && deflected.some((d) => d.reply.startsWith("Hola"));
results["assignment drains the queue in priority order"] = order.every((p, i) => i === 0 || p <= order[i - 1]);
results["no agent is overloaded or given a ticket outside their languages and skills"] = AGENTS.every(
    (a) => a.load <= a.cap && a.assigned.every((t) => a.langs.includes(t.lang) && a.skills.includes(t.intent)),
);
results["queue endpoint reports triage depth"] = queues.triage === triage.length;
const burst = incoming
    .filter((t) => t.id.startsWith("T2"))
    .map((t) => tokens(t.text).join(" "))
    .join(" | ");
results[`every top spike comes from the outage burst (${spikes.take(3).pluck("b").join(", ")})`] =
    spikes.length >= 3 && spikes.take(3).all((x) => burst.includes(x.b));
results["the native BM25 index picks the password article for a login question"] =
    bm25("password reset email never arrives").a.id === "KB-1";

print("ticket-router");
print(
    `  models     language acc ${evalScores.lang.toFixed(3)} | intent acc ${evalScores.intent.toFixed(3)} (outage F1 ${evalScores.outageF1.toFixed(3)}) on ${test.length} held-out tickets`,
);
print(
    `  confusion  ${INTENTS.map((n, i) => `${n}:${confusion[i][i]}/${confusion[i].reduce((a, b) => a + b, 0)}`).join(" ")}`,
);
print(
    `  routing    ${Object.entries(Object.groupBy(decisions, (d) => d.action))
        .map(([k, v]) => `${k} ${v.length}`)
        .join(", ")} (${decisions.length} tickets)`,
);
print(
    `  pii        ${decisions.reduce((s, d) => s + (d.redacted?.length || 0), 0)} items redacted (${[...new Set(decisions.flatMap((d) => d.redacted || []))].join(", ")})`,
);
print(
    `  agents     ${AGENTS.map((a) => `${a.name} ${a.load}/${a.cap}`).join("  ")}; ${unassigned} waiting for capacity`,
);
print(`  example    ${deflected[0].reply.split("\n")[2]}`);
print(
    `  spikes     ${spikes
        .take(4)
        .map((x) => `"${x.b}" x${x.lift.round(0)}`)
        .join(", ")}`,
);
for (const m of [langModel, intentModel]) m.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
