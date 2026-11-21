// Mail client — a desktop mail app on the dyna:ui immediate-mode UI kit, driven headlessly and checked.
// apps:needs dyna:ui
// flags: --std
//
// WHAT IT IS
//   A three-pane mail client: menubar, toolbar with search, a folder tree beside a sortable,
//   virtualised table of 20,000 messages, a reader pane, a compose dialog, toasts and a status bar.
//   With no arguments it drives itself the way a person would — every click lands on a widget found
//   with lastRect() — and checks each outcome against plain JS over the same data: the folder count,
//   the sort order, the search hits, the message sent. Idle frames must be reused (no layout work).
//
// MODULES  dyna:ui (Context, widgets, pixelsInto, stats) · dyna:sdl (only with --window)
//
// RUN      dynajs --std examples/ui/mail-client.js             drives itself headlessly and checks the outcome
//          dynajs --std examples/ui/mail-client.js --window    opens it in a window (needs CONFIG_SDL3)

import * as ui from "dyna:ui";

const N = 20000;
const senders = ["Ada", "Grace", "Alan", "Edsger", "Barbara", "Ken", "Dennis", "Margaret", "Linus", "Guido"];
const subjects = [
    "Quarterly report",
    "Lunch?",
    "Build is red",
    "Release notes",
    "Design review",
    "Re: invoices",
    "Travel plans",
    "Patch v3",
];
const folders = ["Inbox", "Sent", "Drafts", "Archive"];
const mail = Array.from({ length: N }, (_, i) => ({
    from: senders[(i * 7) % senders.length],
    subject: subjects[(i * 13) % subjects.length] + " #" + i,
    date: `2026-${String(1 + (i % 12)).padStart(2, "0")}-${String(1 + (i % 28)).padStart(2, "0")}`,
    size: ((i * 37) % 900) + 3,
    folder: i % 4,
}));
const KEYS = ["from", "subject", "date", "size"];

const state = {
    folder: 0,
    selected: -1,
    sort: { column: 2, descending: true },
    query: "",
    ratio: 0.22,
    ratio2: 0.55,
    to: "",
    subj: "",
    body: "",
    sent: [],
    toasts: 0,
};
const rects = {};
let view = null;

function visibleMail() {
    const key = `${state.folder}|${state.query}|${state.sort.column}|${state.sort.descending}`;
    if (view && view.key === key) return view.rows;
    const q = state.query.toLowerCase();
    const rows = mail.filter(
        (m) =>
            m.folder === state.folder &&
            (!q || m.subject.toLowerCase().includes(q) || m.from.toLowerCase().includes(q)),
    );
    const k = KEYS[state.sort.column] || "date";
    rows.sort((a, b) => (a[k] < b[k] ? -1 : a[k] > b[k] ? 1 : 0) * (state.sort.descending ? -1 : 1));
    view = { key, rows };
    return rows;
}

function app(u) {
    const rec = (name) => {
        rects[name] = u.lastRect();
    };
    const rows = visibleMail();
    u.menubar(() => {
        u.menu("File", () => {
            if (u.menuItem("New message", { shortcut: "Ctrl+N" })) u.openModal("Compose");
            u.menuItem("Quit", { shortcut: "Ctrl+Q" });
        });
        u.menu("Help", () => u.menuItem("About"));
    });
    u.toolbar(() => {
        if (u.button("Compose")) u.openModal("Compose");
        rec("compose");
        u.tooltip("Write a new message");
        u.button("Reply");
        u.button("Forward");
        u.separator();
        state.query = u.search("##search", state.query, { hint: "Search mail", width: 220 });
        rec("search");
    });
    state.ratio = u.split(
        "main",
        state.ratio,
        () => {
            u.tree("Mailboxes", { open: true }, () => {
                folders.forEach((f, i) => {
                    if (u.selectable(f + "##f" + i, state.folder === i) && state.folder !== i) {
                        state.folder = i;
                        state.selected = -1;
                    }
                    rec("folder" + i);
                });
            });
        },
        () => {
            state.ratio2 = u.split(
                "right",
                state.ratio2,
                { vertical: true },
                () => {
                    const t = u.table("Messages", {
                        columns: [
                            { name: "From", width: 90 },
                            { name: "Subject", width: 260 },
                            { name: "Date", width: 90 },
                            { name: "Size", width: 60 },
                        ],
                        rows: rows.length,
                        cell: (r, c) => {
                            const m = rows[r];
                            return c === 0 ? m.from : c === 1 ? m.subject : c === 2 ? m.date : m.size + " KB";
                        },
                        visibleRows: 12,
                        selected: state.selected,
                        sort: state.sort,
                    });
                    rec("table");
                    rects.subjectHeader = u.lastRect(-1, 1);
                    rects.row3 = u.lastRect(3);
                    if (t.sortColumn !== state.sort.column || t.descending !== state.sort.descending)
                        state.sort = { column: t.sortColumn, descending: t.descending };
                    state.selected = t.selected;
                },
                () => {
                    const m = rows[state.selected];
                    if (!m) {
                        u.text("No message selected", { color: 0x9aa0a6ff });
                        return;
                    }
                    u.heading(m.subject);
                    u.row(() => {
                        u.avatar(m.from);
                        u.column(() => {
                            u.label(m.from);
                            u.text(m.date, { color: 0x9aa0a6ff });
                        });
                    });
                    u.separator();
                    u.text(("Hello,\n\nThis is the body of " + m.subject + ". ").repeat(4), { wrap: true });
                },
            );
        },
    );
    u.modal("Compose", { width: 420 }, () => {
        u.property("To", () => {
            state.to = u.textField("##to", state.to, { hint: "name@example.com" });
            rec("to");
        });
        u.property("Subject", () => {
            state.subj = u.textField("##subj", state.subj);
            rec("subj");
        });
        state.body = u.textArea("##body", state.body, { rows: 6 });
        u.row(() => {
            if (u.button("Send")) {
                state.sent.push({ to: state.to, subject: state.subj });
                state.toasts++;
                u.toast("Sent to " + state.to);
                u.closePopup();
            }
            rec("send");
            if (u.button("Cancel")) u.closePopup();
        });
    });
    u.statusbar(() => {
        u.label(`${rows.length} messages`);
        u.spacer();
        u.label(`${state.sent.length} sent`);
    });
}

// ---- headless driver: a person's clicks, typing and idle time, one frame behind as the kit is ----
const W = 1100,
    H = 720;
const c = new ui.Context({ width: W, height: H, clock: "manual" });
let now = 0;
const frame = () => {
    now += 1 / 60;
    c.setTime(now);
    return c.frame(app);
};
const mid = (r) => [r.x + r.width / 2, r.y + r.height / 2];
const click = ([x, y]) => {
    c.pointer({ x, y, buttons: 1 });
    frame();
    c.pointer({ x, y, buttons: 0 });
    frame();
    frame();
};
const type = (s) => {
    c.type(s);
    frame();
};

const results = {};
if (scriptArgs.includes("--window")) await runWindow();
else {
    for (let k = 0; k < 3; k++) frame();

    click(mid(rects.folder1));
    const sentFolder = mail.filter((m) => m.folder === 1).length;
    results["the Sent folder shows every message filed there"] =
        state.folder === 1 && visibleMail().length === sentFolder;

    click(mid(rects.subjectHeader));
    const subj = visibleMail().map((m) => m.subject);
    const sorted = [...subj].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
    results["a click on the Subject header sorts by subject, ascending"] =
        state.sort.column === 1 && !state.sort.descending && subj.every((s, i) => s === sorted[i]);
    click(mid(rects.subjectHeader));
    results["a second click reverses it"] =
        state.sort.descending && visibleMail()[0].subject === sorted[sorted.length - 1];

    click(mid(rects.row3));
    results["a click on the fourth row selects it and the reader shows it"] = state.selected === 3;

    click(mid(rects.search));
    type("Grace");
    const hits = mail.filter(
        (m) => m.folder === 1 && (m.subject.toLowerCase().includes("grace") || m.from.toLowerCase().includes("grace")),
    ).length;
    results["typing in search filters the folder to the matching messages"] =
        state.query === "Grace" && visibleMail().length === hits && hits > 0;

    click(mid(rects.compose));
    results["Compose opens the dialog with its fields"] = !!rects.to && !!rects.send;
    click(mid(rects.to));
    type("ada@example.com");
    click(mid(rects.subj));
    type("Hello there");
    click(mid(rects.send));
    results["Send records the message once, with what was typed"] =
        state.sent.length === 1 && state.sent[0].to === "ada@example.com" && state.sent[0].subject === "Hello there";
    results["and confirms it with a toast"] = state.toasts === 1;

    const before = c.stats().reusedFrames;
    for (let k = 0; k < 30; k++) {
        c.setTime(now);
        c.frame(app);
    }
    results["idle frames reuse the last layout instead of recomputing it"] = c.stats().reusedFrames - before >= 29;

    const px = c.pixelsInto(new Uint8Array(c.pixelWidth * c.pixelHeight * 4));
    let ink = 0;
    for (let i = 0; i < px.length; i += 4) if (px[i] !== px[0] || px[i + 1] !== px[1] || px[i + 2] !== px[2]) ink++;
    results["the frame is drawn (more than a tenth of the window differs from the background)"] = ink > (W * H) / 10;
    c.close();
    check(results);
    print(`mail-client: ${Object.keys(results).length} checks passed over ${N} messages`);
}

async function runWindow() {
    let sdl;
    try {
        sdl = await import("dyna:sdl");
    } catch (e) {
        throw new Error("--window needs dyna:sdl (build with CONFIG_SDL3=y)");
    }
    sdl.init(["video"]);
    const win = new sdl.Window("Mail", W, H, { resizable: true, highDensity: true });
    const r = new sdl.Renderer(win);
    const w = new ui.Context({ target: r });
    for (let running = true; running;) {
        for (const e of sdl.pollEvents()) if (e.type === "quit") running = false;
        w.frame(app);
        w.idle(1);
    }
    w.close();
    r.close();
    win.close();
    sdl.quit();
}

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
