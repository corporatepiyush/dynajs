// Kanban board — a project board on dyna:ui: scrolling columns, drag and drop, context menus, filters and a validated dialog.
// apps:needs dyna:ui
// flags: --std
//
// WHAT IT IS
//   Four columns of task cards (To do, Doing, Review, Done), each in its own scroll area. A card can be
//   dragged onto another column; a right press on a card opens a context menu that moves it to another
//   column or deletes it; label chips and a
//   search box filter every column at once; "New task" opens a dialog that refuses an empty title and
//   files a valid task under To do. With no arguments it drives itself the way a person would — every
//   press lands on a widget found with lastRect() — and checks each outcome against plain JS over the
//   same task list: which column a card is in, which cards a filter keeps, how far a wheel scrolls.
//
// MODULES  dyna:ui (Context, scroll, dragSource, dropTarget, contextMenu, chip, modal, combo, rating, toast) · dyna:sdl (only with --window)
//
// RUN      dynajs --std examples/ui/kanban-board.js             drives itself headlessly and checks the outcome
//          dynajs --std examples/ui/kanban-board.js --window    opens it in a window (needs CONFIG_SDL3)

import * as ui from "dyna:ui";

const COLUMNS = ["To do", "Doing", "Review", "Done"];
const LABELS = ["bug", "feature", "docs", "infra"];
const PEOPLE = ["Ada", "Grace", "Alan", "Edsger", "Barbara"];
const WORDS = ["auth", "cache", "parser", "release", "login", "billing", "search", "export", "metrics", "onboarding"];
let nextId = 0;
const tasks = Array.from({ length: 64 }, (_, i) => ({
    id: nextId++,
    title: `${WORDS[(i * 7) % WORDS.length]} ${["fix", "rework", "audit", "ship"][i % 4]} #${i}`,
    column: i % 4,
    label: (i * 3) % LABELS.length,
    owner: PEOPLE[(i * 5) % PEOPLE.length],
    priority: 1 + (i % 5),
}));
const state = {
    query: "",
    labels: LABELS.map(() => false),
    draft: { title: "", owner: 0, priority: 3 },
    refused: 0,
    created: 0,
    deleted: 0,
    selected: -1,
    dropped: 0,
    dragging: null,
};
const rects = {};

function visible(column) {
    const q = state.query.toLowerCase();
    const anyLabel = state.labels.some(Boolean);
    return tasks.filter(
        (t) =>
            t.column === column &&
            (!q || t.title.toLowerCase().includes(q) || t.owner.toLowerCase().includes(q)) &&
            (!anyLabel || state.labels[t.label]),
    );
}

function app(u) {
    const rec = (name) => {
        rects[name] = u.lastRect();
    };
    u.toolbar(() => {
        if (u.button("New task")) {
            state.draft = { title: "", owner: 0, priority: 3 };
            u.openModal("New task");
        }
        rec("new");
        u.separator();
        state.query = u.search("##q", state.query, { hint: "Filter by title or owner", width: 220 });
        rec("search");
        u.separator();
        LABELS.forEach((l, i) => {
            state.labels[i] = u.chip(l, state.labels[i]);
            rec("chip" + i);
        });
    });
    u.row({ gap: 8, width: "grow", height: "grow" }, () => {
        COLUMNS.forEach((name, ci) => {
            const cards = visible(ci);
            u.column({ id: "lane" + ci, width: { grow: 1 }, height: "grow", gap: 4 }, () => {
                u.row(() => {
                    u.heading(name);
                    u.badge(String(cards.length));
                    rec("count" + ci);
                });
                u.scroll("col" + ci, { height: 420 }, () => {
                    for (const t of cards)
                        u.id(t.id, () => {
                            u.box({ width: "grow", padding: 6, gap: 4, radius: 6, fill: 0x2a2f3aff }, () => {
                                if (u.selectable(t.title, state.selected === t.id)) state.selected = t.id;
                                rec("card" + t.id);
                                u.dragSource(t.id, { label: t.title });
                                u.contextMenu("menu", () => {
                                    COLUMNS.forEach((target, ti) => {
                                        if (ti !== t.column && u.menuItem("Move to " + target)) t.column = ti;
                                        rec("move" + t.id + "_" + ti);
                                    });
                                    if (u.menuItem("Delete")) {
                                        tasks.splice(tasks.indexOf(t), 1);
                                        state.deleted++;
                                    }
                                    rec("delete" + t.id);
                                });
                                u.row(() => {
                                    u.badge(LABELS[t.label]);
                                    u.avatar(t.owner, { size: 18 });
                                    u.text("P" + t.priority, { color: 0x9aa0a6ff });
                                });
                            });
                        });
                });
                rec("scroll" + ci);
            });
            rec("lane" + ci);
            const id = u.dropTarget();
            const moved = id === null ? undefined : tasks.find((t) => t.id === id);
            if (moved && moved.column !== ci) {
                moved.column = ci;
                state.dropped++;
            }
        });
    });
    u.modal("New task", { width: 380 }, () => {
        u.property("Title", () => {
            state.draft.title = u.textField("##title", state.draft.title, { hint: "What needs doing?" });
            rec("title");
        });
        u.property("Owner", () => {
            state.draft.owner = u.combo("##owner", PEOPLE, state.draft.owner);
            rec("owner");
            rects.owner3 = u.lastRect(3);
        });
        u.property("Priority", () => {
            state.draft.priority = u.rating("##prio", state.draft.priority, { max: 5 });
            rec("priority");
            rects.star5 = u.lastRect(4);
        });
        if (state.refused && !state.draft.title.trim()) u.text("A task needs a title", { color: 0xe06c75ff });
        u.row(() => {
            if (u.button("Create")) {
                const title = state.draft.title.trim();
                if (!title) state.refused++;
                else {
                    tasks.push({
                        id: nextId++,
                        title,
                        column: 0,
                        label: 1,
                        owner: PEOPLE[state.draft.owner],
                        priority: state.draft.priority,
                    });
                    state.created++;
                    u.toast("Created " + title);
                    u.closePopup();
                }
            }
            rec("create");
            if (u.button("Cancel")) u.closePopup();
        });
    });
    state.dragging = u.dragging;
    u.statusbar(() => {
        u.label(`${tasks.length} tasks`);
        u.spacer();
        u.label(`${state.created} created, ${state.deleted} deleted`);
    });
}

// ---- headless driver ----
const W = 1200,
    H = 640;
const c = new ui.Context({ width: W, height: H, clock: "manual" });
let now = 0;
const frame = () => {
    now += 1 / 60;
    c.setTime(now);
    return c.frame(app);
};
const mid = (r) => [r.x + r.width / 2, r.y + r.height / 2];
const press = ([x, y], buttons = 1) => {
    c.pointer({ x, y, buttons });
    frame();
    c.pointer({ x, y, buttons: 0 });
    frame();
    frame();
};
const results = {};
if (scriptArgs.includes("--window")) await runWindow();
else {
    for (let k = 0; k < 3; k++) frame();
    results["each column's badge counts the cards filed there"] = COLUMNS.every(
        (_, ci) => visible(ci).length === tasks.filter((t) => t.column === ci).length && visible(ci).length === 16,
    );

    const t0 = visible(0)[0];
    press(mid(rects["card" + t0.id]), 2);
    results["a right press on a card opens its menu with a move for every other column"] =
        !!rects["move" + t0.id + "_3"] && !!rects["delete" + t0.id];
    press(mid(rects["move" + t0.id + "_3"]));
    results["Move to Done refiles exactly that card"] =
        t0.column === 3 && visible(0).length === 15 && visible(3).length === 17 && visible(3).includes(t0);

    frame();
    const dragged = visible(0)[1],
        sel = state.selected,
        [sx, sy] = mid(rects["card" + dragged.id]),
        [tx, ty] = mid(rects.lane2);
    c.pointer({ x: sx, y: sy, buttons: 1 });
    frame();
    c.pointer({ x: sx + 30, y: sy + 10, buttons: 1 });
    frame();
    const lifted = state.dragging;
    c.pointer({ x: tx, y: ty, buttons: 1 });
    frame();
    frame();
    c.pointer({ x: tx, y: ty, buttons: 0 });
    frame();
    frame();
    results["dragging a card onto Review files it there and nowhere else"] =
        lifted === dragged.id &&
        dragged.column === 2 &&
        state.dropped === 1 &&
        visible(0).length === 14 &&
        visible(2).length === 17;
    results["the drag's release does not select the card"] = state.selected === sel;

    const doomed = visible(1)[2];
    press(mid(rects["card" + doomed.id]), 2);
    press(mid(rects["delete" + doomed.id]));
    results["Delete removes the card and nothing else"] =
        !tasks.includes(doomed) && tasks.length === 63 && visible(1).length === 15 && state.deleted === 1;

    const first = visible(2)[0],
        y0 = rects["card" + first.id].y,
        sc = rects.scroll2;
    c.pointer({ x: sc.x + sc.width / 2, y: sc.y + sc.height / 2, buttons: 0, wheelY: 3 });
    frame();
    frame();
    results["the wheel cannot scroll a column above its top"] = rects["card" + first.id].y === y0;
    c.pointer({ x: sc.x + sc.width / 2, y: sc.y + sc.height / 2, buttons: 0, wheelY: -3 });
    frame();
    frame();
    const y1 = rects["card" + first.id].y;
    results["scrolling down over one column moves its cards up"] = y1 < y0;
    const other = visible(1)[0];
    results["and leaves the other columns where they were"] = rects["card" + other.id].y === y0;
    c.pointer({ x: sc.x + sc.width / 2, y: sc.y + sc.height / 2, buttons: 0, wheelY: 3 });
    frame();
    frame();
    results["scrolling back up by the same amount restores it"] = rects["card" + first.id].y === y0;

    press(mid(rects.chip0));
    results["the bug chip keeps only bugs, in every column"] = COLUMNS.every((_, ci) =>
        visible(ci).every((t) => t.label === 0),
    );
    const bugs = tasks.filter((t) => t.label === 0).length;
    results["and keeps all of them"] = COLUMNS.reduce((n, _, ci) => n + visible(ci).length, 0) === bugs;
    press(mid(rects.chip0));

    press(mid(rects.search));
    c.type("auth");
    frame();
    frame();
    const hits = tasks.filter((t) => t.title.includes("auth")).length;
    results["typing a word keeps the cards whose title holds it"] =
        COLUMNS.reduce((n, _, ci) => n + visible(ci).length, 0) === hits &&
        COLUMNS.every((_, ci) => visible(ci).every((t) => t.title.includes("auth")));
    for (let k = 0; k < 4; k++) c.key("backspace");
    frame();
    frame();
    results["deleting the query restores every card"] =
        state.query === "" && COLUMNS.reduce((n, _, ci) => n + visible(ci).length, 0) === tasks.length;

    press(mid(rects.new));
    press(mid(rects.create));
    results["Create with an empty title is refused, and the dialog stays"] =
        state.refused === 1 && state.created === 0 && !!rects.title;
    press(mid(rects.title));
    c.type("Write the release notes");
    frame();
    press(mid(rects.owner));
    press(mid(rects.owner3));
    press(mid(rects.star5));
    press(mid(rects.create));
    const made = tasks[tasks.length - 1];
    results["a titled task is filed under To do with the owner and priority chosen"] =
        state.created === 1 &&
        made.title === "Write the release notes" &&
        made.column === 0 &&
        made.owner === PEOPLE[3] &&
        made.priority === 5;

    const before = c.stats().reusedFrames;
    for (let k = 0; k < 30; k++) {
        c.setTime(now);
        c.frame(app);
    }
    results["an idle board reuses its last layout"] = c.stats().reusedFrames - before >= 29;
    c.close();
    check(results);
    print(`kanban-board: ${Object.keys(results).length} checks passed over ${tasks.length} tasks`);
}

async function runWindow() {
    let sdl;
    try {
        sdl = await import("dyna:sdl");
    } catch (e) {
        throw new Error("--window needs dyna:sdl (build with CONFIG_SDL3=y)");
    }
    sdl.init(["video"]);
    const win = new sdl.Window("Kanban", W, H, { resizable: true, highDensity: true });
    const r = new sdl.Renderer(win);
    const w = new ui.Context({ target: r });
    for (let running = true; running;) {
        for (const e of sdl.pollEvents()) if (e.type === "quit") running = false;
        w.frame(app);
        w.idle(1 / 30);
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
