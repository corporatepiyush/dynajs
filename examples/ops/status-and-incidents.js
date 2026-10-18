// Status page and incident management — synthetic monitors, component health with flap damping, incidents with
// an on-call rotation and escalation policy, a public status page with an Atom feed, uptime SLAs and postmortems.
//
// WHAT IT IS
//   What sits behind status.example.com and the pager, end to end:
//
//   monitors     synthetic checks run against real HTTP services every "minute": status code, body assertion
//                (a JSON pointer must equal a value), latency budget; a check must fail 3 times in a row to count
//                (flap damping) and pass 2 times to recover
//   components   API, Checkout, Search, Webhooks — each derived from its monitors as operational / degraded /
//                partial outage / major outage
//   incidents    a component going down opens an incident automatically; the on-call engineer is found from a
//                weekly rotation (RRule) with overrides; unacknowledged pages escalate to the secondary after 5
//                minutes and to the manager after 15; updates are posted with a state (investigating -> identified
//                -> monitoring -> resolved)
//   maintenance  scheduled windows suppress paging and show on the page as maintenance, not outage
//   publishing   the status page (HTML from a template), a JSON API, and an Atom feed of incident updates
//   reporting    per-component uptime for the month, MTTA / MTTR from the incident log, SLA credits owed, and a
//                postmortem draft rendered from Markdown
//   intervals    the fault plan and the maintenance windows are IntervalTrees (a lookup per check is a stabbing
//                query, not a scan); each component's downtime is a RangeSet of [from, to) minutes, so overlapping
//                observations merge themselves and uptime is the covered length; the minutes worth checking every
//                minute are a RangeSet too
//
// MODULES  dyna:http (App, HTTPServer) · dyna:time (RRule, formatRFC3339) · dyna:json (Pointer get) · dyna:xml (XmlWriter)
//          · dyna:html (Template, MarkdownToHTML) · dyna:dataframe · dyna:uuid · dyna:cli (Table)
//          · dyna:structures (IntervalTree, RangeSet)
//
// RUN      dynajs examples/ops/status-and-incidents.js     simulates a month of checks with three incidents and a maintenance

import { App } from "dyna:http";
import { RRule, formatRFC3339 } from "dyna:time";
import { Pointer as JSONPointer } from "dyna:json";
import { XmlWriter, XMLParse } from "dyna:xml";
import { Template, MarkdownToHTML } from "dyna:html";
import { DataFrame } from "dyna:dataframe";
import { v7 as uuid7 } from "dyna:uuid";
import { Table } from "dyna:cli";
import { IntervalTree, RangeSet } from "dyna:structures";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const MIN = 60_000,
    T0 = Date.UTC(2026, 3, 1),
    DAYS = 30;
let clock = T0;

// =============================================================================
// The services being watched (real HTTP servers whose behaviour follows a fault plan)
// =============================================================================
const FAULTS = [
    { service: "api", from: T0 + (4 * 1440 + 600) * MIN, to: T0 + (4 * 1440 + 642) * MIN, kind: "down" },
    { service: "search", from: T0 + (11 * 1440 + 200) * MIN, to: T0 + (11 * 1440 + 225) * MIN, kind: "slow" },
    { service: "checkout", from: T0 + (19 * 1440 + 900) * MIN, to: T0 + (19 * 1440 + 925) * MIN, kind: "wrong" },
    { service: "webhooks", from: T0 + (25 * 1440 + 120) * MIN, to: T0 + (25 * 1440 + 200) * MIN, kind: "down" }, // inside maintenance
    { service: "api", from: T0 + (8 * 1440 + 300) * MIN, to: T0 + (8 * 1440 + 302) * MIN, kind: "down" }, // a 2-minute blip
];
const faultTree = new IntervalTree();
for (const f of FAULTS) faultTree.insert(f.from, f.to - 1, f);
const fault = (svc) =>
    faultTree
        .at(clock)
        .map(([, , f]) => f)
        .find((f) => f.service === svc);
const svc = new App({ port: 0 });
svc.get("/api/health", () =>
    fault("api") ? { status: 503, body: "upstream unavailable" } : { status: "ok", version: "4.12.0", db: "ok" },
);
svc.get("/checkout/health", () => ({
    status: fault("checkout") ? "degraded" : "ok",
    payments: fault("checkout") ? "failing" : "ok",
}));
svc.get("/search/q", async () => {
    if (fault("search")) await new Promise((r) => setTimeout(r, 90));
    return { hits: 42 };
});
svc.get("/webhooks/health", () => (fault("webhooks") ? { status: 500, body: "queue offline" } : { status: "ok" }));
svc.start();
const svcBase = `http://127.0.0.1:${svc.port}`;

// =============================================================================
// Monitors and components
// =============================================================================
const MONITORS = [
    {
        id: "api-health",
        component: "API",
        url: "/api/health",
        expect: { status: 200, pointer: "/db", equals: "ok" },
        budgetMs: 1000,
    },
    {
        id: "checkout-health",
        component: "Checkout",
        url: "/checkout/health",
        expect: { status: 200, pointer: "/payments", equals: "ok" },
        budgetMs: 1000,
    },
    { id: "search-latency", component: "Search", url: "/search/q", expect: { status: 200 }, budgetMs: 60 },
    { id: "webhooks-health", component: "Webhooks", url: "/webhooks/health", expect: { status: 200 }, budgetMs: 1000 },
];
const COMPONENTS = ["API", "Checkout", "Search", "Webhooks"];
const monitorState = new Map(MONITORS.map((m) => [m.id, { up: true, fails: 0, passes: 0 }]));
const downtime = new Map(COMPONENTS.map((c) => [c, new RangeSet()])); // component -> minutes it was down
const downMinutes = (c) =>
    downtime
        .get(c)
        .ranges()
        .reduce((s, [lo, hi]) => s + hi - lo, 0);
async function runCheck(m) {
    const t = performance.now();
    let ok = true,
        reason = "";
    try {
        const r = await fetch(svcBase + m.url);
        const text = await r.text(),
            ms = performance.now() - t;
        if (r.status !== m.expect.status) {
            ok = false;
            reason = `HTTP ${r.status}`;
        } else if (m.expect.pointer && JSONPointer.get(JSON.parse(text), m.expect.pointer) !== m.expect.equals) {
            ok = false;
            reason = `${m.expect.pointer} != ${m.expect.equals}`;
        } else if (ms > m.budgetMs) {
            ok = false;
            reason = `slow: ${ms.toFixed(0)} ms > ${m.budgetMs} ms`;
        }
    } catch (e) {
        ok = false;
        reason = e.message;
    }
    const s = monitorState.get(m.id);
    if (ok) {
        s.passes++;
        s.fails = 0;
        if (!s.up && s.passes >= 2) {
            s.up = true;
            return { change: "up", m };
        }
    } else {
        s.fails++;
        s.passes = 0;
        s.reason = reason;
        if (s.up && s.fails >= 3) {
            s.up = false;
            return { change: "down", m, reason };
        }
    }
    return null;
}
const componentStatus = (c) =>
    MONITORS.filter((m) => m.component === c).every((m) => monitorState.get(m.id).up)
        ? "operational"
        : c === "Search"
          ? "degraded"
          : c === "Checkout"
            ? "partial_outage"
            : "major_outage";

// =============================================================================
// On-call: weekly rotation with overrides, escalation policy
// =============================================================================
const ROTATION = ["amara", "bo", "chen", "dana"];
const rotationStart = new RRule({
    freq: "WEEKLY",
    byweekday: ["MO"],
    dtstart: new Date(T0 - 2 * 86400_000 + 9 * 3600_000),
});
const OVERRIDES = [{ from: T0 + 18 * 86400_000, to: T0 + 21 * 86400_000, who: "eli" }];
function onCall(t) {
    const o = OVERRIDES.find((x) => t >= x.from && t < x.to);
    if (o) return o.who;
    const weeks = rotationStart.between(new Date(T0 - 7 * 86400_000), new Date(t), true).length;
    return ROTATION[(weeks - 1 + ROTATION.length) % ROTATION.length];
}
const ESCALATION = [
    { afterMin: 0, target: "primary" },
    { afterMin: 5, target: "secondary" },
    { afterMin: 15, target: "manager" },
];
const maintenance = [
    {
        component: "Webhooks",
        from: T0 + (25 * 1440 + 60) * MIN,
        to: T0 + (25 * 1440 + 240) * MIN,
        title: "Queue cluster upgrade",
    },
];
const maintenanceTree = new IntervalTree();
for (const w of maintenance) maintenanceTree.insert(w.from, w.to - 1, w);
const inMaintenance = (c, t) =>
    maintenanceTree
        .at(t)
        .map(([, , w]) => w)
        .find((w) => w.component === c);

// =============================================================================
// Incidents
// =============================================================================
const incidents = [],
    pages = [];
function openIncident(component, reason) {
    const inc = {
        id: uuid7(),
        component,
        title: `${component}: ${reason}`,
        opened: clock,
        acked: null,
        resolved: null,
        state: "investigating",
        updates: [],
        pagedTo: [],
        impact: componentStatus(component),
    };
    inc.updates.push({
        at: clock,
        state: "investigating",
        text: `We are investigating elevated errors affecting ${component}.`,
    });
    incidents.push(inc);
    page(inc, 0);
    return inc;
}
function page(inc, level) {
    const primary = onCall(clock),
        secondary = ROTATION[(ROTATION.indexOf(primary) + 1) % ROTATION.length];
    const who = { primary, secondary, manager: "maya" }[ESCALATION[level].target];
    inc.pagedTo.push({ at: clock, who, level });
    pages.push({ at: clock, incident: inc.id, who });
}
function tickIncidents() {
    for (const inc of incidents.filter((i) => !i.resolved)) {
        const age = (clock - inc.opened) / MIN;
        const ackAfter = inc.component === "Search" ? 17 : 3; // the Search page goes unanswered for a while
        if (!inc.acked && age >= ackAfter) {
            inc.acked = clock;
            inc.state = "identified";
            inc.updates.push({
                at: clock,
                state: "identified",
                text: `${inc.pagedTo.at(-1).who} is working on it; cause identified.`,
            });
        }
        for (let l = 1; l < ESCALATION.length; l++)
            if (!inc.acked && age >= ESCALATION[l].afterMin && !inc.pagedTo.some((p) => p.level === l)) page(inc, l);
    }
}
function resolveIncident(component) {
    const inc = incidents.find((i) => i.component === component && !i.resolved);
    if (!inc) return;
    inc.state = "monitoring";
    inc.updates.push({ at: clock, state: "monitoring", text: "A fix has been deployed and we are monitoring." });
    inc.resolved = clock;
    inc.state = "resolved";
    inc.updates.push({ at: clock + 10 * MIN, state: "resolved", text: "This incident has been resolved." });
}

// =============================================================================
// Simulation: one check round per minute around fault windows, every 30 minutes otherwise
// =============================================================================
let checks = 0,
    suppressed = 0;
const watchClosely = new RangeSet(); // minutes checked every minute rather than every 30
for (const f of FAULTS) watchClosely.add(f.from - 10 * MIN, f.to + 20 * MIN);
for (const w of maintenance) watchClosely.add(w.from, w.to);
const interesting = (t) => watchClosely.contains(t);
for (clock = T0; clock < T0 + DAYS * 1440 * MIN;) {
    const step = interesting(clock) ? MIN : 30 * MIN;
    for (const m of MONITORS) {
        checks++;
        const ev = await runCheck(m);
        if (!ev) continue;
        if (ev.change === "down") {
            if (inMaintenance(m.component, clock)) {
                suppressed++;
                continue;
            }
            openIncident(m.component, ev.reason);
        } else resolveIncident(m.component);
    }
    for (const c of COMPONENTS)
        if (componentStatus(c) !== "operational" && !inMaintenance(c, clock))
            downtime.get(c).add((clock - T0) / MIN, (clock - T0 + step) / MIN);
    tickIncidents();
    clock += step;
}

// =============================================================================
// Publishing: HTML status page, JSON, Atom feed
// =============================================================================
const PAGE = new Template(`<!doctype html><html><head><meta charset="utf-8"><title>Example Status</title></head><body>
<h1 class="{{overallClass}}">{{overall}}</h1>
<ul class="components">{{#components}}<li class="{{status}}">{{name}} <span>{{label}}</span> <small>{{uptime}} uptime</small></li>{{/components}}</ul>
{{#maintenance}}<section class="maintenance"><h2>Scheduled maintenance</h2><p>{{title}} ({{component}}), {{from}} – {{to}} UTC</p></section>{{/maintenance}}
<h2>Past incidents</h2>{{#incidents}}<article><h3>{{title}}</h3>{{#updates}}<p><b>{{state}}</b> {{at}} — {{text}}</p>{{/updates}}</article>{{/incidents}}
</body></html>`);
const LABEL = {
    operational: "Operational",
    degraded: "Degraded performance",
    partial_outage: "Partial outage",
    major_outage: "Major outage",
};
const minutesInMonth = DAYS * 1440;
const uptime = (c) => 100 * (1 - downMinutes(c) / minutesInMonth);
const iso = (t) =>
    formatRFC3339(Math.floor(t / 1000))
        .replace("T", " ")
        .slice(0, 16);
function statusJson() {
    const comps = COMPONENTS.map((c) => ({
        name: c,
        status: componentStatus(c),
        uptime: Number(uptime(c).toFixed(3)),
    }));
    const worst = comps.find((c) => c.status !== "operational");
    return {
        page: "Example",
        overall: worst ? "Some systems are experiencing issues" : "All Systems Operational",
        components: comps,
        incidents: incidents.map(({ id, title, state, opened, resolved }) => ({
            id,
            title,
            state,
            opened: iso(opened),
            resolved: resolved && iso(resolved),
        })),
    };
}
function atom() {
    const w = new XmlWriter({ indent: 1 });
    w.open("feed", { xmlns: "http://www.w3.org/2005/Atom" });
    for (const [k, v] of [
        ["title", "Example status"],
        ["id", "tag:status.example.com,2026:feed"],
        ["updated", formatRFC3339(Math.floor(clock / 1000))],
    ]) {
        w.open(k);
        w.text(v);
        w.close();
    }
    for (const inc of [...incidents].reverse())
        for (const u of [...inc.updates].reverse()) {
            w.open("entry");
            for (const [k, v] of [
                ["id", `tag:status.example.com,2026:${inc.id}:${u.at}`],
                ["title", `[${u.state}] ${inc.title}`],
                ["updated", formatRFC3339(Math.floor(u.at / 1000))],
                ["summary", u.text],
            ]) {
                w.open(k);
                w.text(v);
                w.close();
            }
            w.close();
        }
    w.close();
    return w.finish();
}
const site = new App({ port: 0 });
site.get("/", () => {
    const s = statusJson();
    return {
        contentType: "text/html",
        body: PAGE.render({
            overall: s.overall,
            overallClass: s.components.some((c) => c.status !== "operational") ? "issues" : "ok",
            components: s.components.map((c) => ({ ...c, label: LABEL[c.status], uptime: `${c.uptime.toFixed(2)}%` })),
            maintenance: maintenance.map((w) => ({ ...w, from: iso(w.from), to: iso(w.to) })),
            incidents: [...incidents].reverse().map((i) => ({
                title: i.title,
                updates: [...i.updates].reverse().map((u) => ({ ...u, at: iso(u.at) })),
            })),
        }),
    };
});
site.get("/api/v2/summary.json", () => statusJson());
site.get("/history.atom", () => ({ contentType: "application/atom+xml", body: atom() }));
site.start();
const html = await (await fetch(`http://127.0.0.1:${site.port}/`)).text();
const summary = await (await fetch(`http://127.0.0.1:${site.port}/api/v2/summary.json`)).json();
const feed = XMLParse(await (await fetch(`http://127.0.0.1:${site.port}/history.atom`)).text());

// =============================================================================
// Reporting: uptime, MTTA/MTTR, SLA credits, postmortem
// =============================================================================
const df = new DataFrame({
    mtta: Float64Array.from(incidents.map((i) => (i.acked - i.opened) / MIN)),
    mttr: Float64Array.from(incidents.map((i) => (i.resolved - i.opened) / MIN)),
});
const SLA = [
    [99.9, 0],
    [99.0, 10],
    [95.0, 25],
    [0, 100],
];
const credit = (u) => SLA.find(([min]) => u >= min)[1];
const worst = [...incidents].sort((a, b) => b.resolved - b.opened - (a.resolved - a.opened))[0];
const postmortem = MarkdownToHTML(`# Postmortem: ${worst.title}

**Duration:** ${((worst.resolved - worst.opened) / MIN).toFixed(0)} minutes · **Impact:** ${LABEL[worst.impact]} · **Paged:** ${worst.pagedTo.map((p) => p.who).join(" → ")}

## Timeline (UTC)
${worst.updates.map((u) => `- ${iso(u.at)} — *${u.state}* — ${u.text}`).join("\n")}

## What went well
- Detection took ${3} consecutive failed checks, so no false page for the 2-minute blip on day 8.

## Action items
- [ ] Add a canary check for ${worst.component} dependencies
- [ ] Shorten the escalation timer for ${worst.component}
`);

const results = {};
results[`three incidents opened (${incidents.map((i) => i.component)})`] =
    incidents.length === 3 && incidents.map((i) => i.component).join() === "API,Search,Checkout";
results["the 2-minute blip did not open an incident (flap damping)"] = !incidents.some(
    (i) => i.opened >= FAULTS[4].from && i.opened < FAULTS[4].to + 10 * MIN,
);
results["the outage inside the maintenance window paged nobody"] =
    suppressed === 1 && !incidents.some((i) => i.component === "Webhooks");
const searchInc = incidents.find((i) => i.component === "Search");
results[`the unanswered Search page escalated to ${searchInc.pagedTo.map((p) => p.who).join(" -> ")}`] =
    searchInc.pagedTo.map((p) => p.level).join() === "0,1,2";
results["the override puts eli on call during days 18–21"] =
    incidents.find((i) => i.component === "Checkout").pagedTo[0].who === "eli";
results["the weekly rotation pages different engineers in different weeks"] =
    new Set(incidents.map((i) => i.pagedTo[0].who)).size >= 2;
results["a body assertion failure is reported with its JSON pointer"] = incidents
    .find((i) => i.component === "Checkout")
    .title.includes("/payments != ok");
results[`uptime: API ${uptime("API").toFixed(3)}%, Webhooks ${uptime("Webhooks")}% (maintenance excluded)`] =
    uptime("API") < 100 && uptime("API") > 99.8 && uptime("Webhooks") === 100;
results[
    "the API's downtime RangeSet covers the 42-minute outage plus the detection/recovery lag, as one merged range"
] = downtime.get("API").ranges().length === 1 && downMinutes("API") >= 40 && downMinutes("API") <= 50;
results["the interval trees agree with a scan of the plan"] = [
    T0,
    FAULTS[0].from,
    FAULTS[0].to - 1,
    FAULTS[0].to,
    FAULTS[2].from + 5 * MIN,
].every((t) => {
    const saved = clock;
    clock = t;
    const same = ["api", "search", "checkout", "webhooks"].every(
        (sv) => fault(sv) === FAULTS.find((f) => f.service === sv && t >= f.from && t < f.to),
    );
    clock = saved;
    return same;
});
results["the status page and JSON API render"] =
    html.includes("All Systems Operational") &&
    html.includes("Queue cluster upgrade") &&
    summary.components.length === 4;
results["the Atom feed carries every incident update"] =
    feed.children.filter((c) => c.name === "entry").length === incidents.reduce((n, i) => n + i.updates.length, 0);
results["a postmortem draft renders from Markdown"] =
    postmortem.includes("<h2>Timeline (UTC)</h2>") && /<li>.*identified/.test(postmortem);

print("status-and-incidents");
print(
    `  checks     ${checks} synthetic checks over ${DAYS} days; ${pages.length} pages sent; ${suppressed} suppressed by maintenance`,
);
print(
    table([
        ["component", "uptime", "downtime", "SLA credit"],
        ...COMPONENTS.map((c) => [
            c,
            `${uptime(c).toFixed(3)}%`,
            `${downMinutes(c).toFixed(0)} min`,
            `${credit(uptime(c))}%`,
        ]),
    ]),
);
print(
    table([
        ["opened", "component", "paged", "ack", "resolved", "cause"],
        ...incidents.map((i) => [
            iso(i.opened),
            i.component,
            i.pagedTo.map((p) => p.who).join(">"),
            `${((i.acked - i.opened) / MIN).toFixed(0)} min`,
            `${((i.resolved - i.opened) / MIN).toFixed(0)} min`,
            i.title.split(": ")[1],
        ]),
    ]),
);
print(
    `  MTTA ${df.MEAN("mtta").toFixed(1)} min, MTTR ${df.MEAN("mttr").toFixed(1)} min; postmortem drafted for "${worst.title}"`,
);
svc.close();
site.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
