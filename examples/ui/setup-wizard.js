// Setup wizard — a responsive, validated four-step flow on dyna:ui that keeps its state across resizes.
// apps:needs dyna:ui
// flags: --std
//
// WHAT IT IS
//   An onboarding flow in four steps (Account, Preferences, Schedule, Review) under a stepper. Each
//   step validates before Next lets the user on: an empty name or a malformed email keeps Account
//   open and says why, and the stepper refuses to jump past a step that does not validate. The
//   layout follows the window's size class: below 600 points it is a phone (a bottom navigation
//   bar), from 840 a desktop (a navigation rail on the left, the summary beside the form). Turning
//   the "phone" sideways or resizing the desktop mid-flow keeps every value entered. Finish files a
//   settings object, and the driver checks it field by field against what it typed and clicked.
//
// MODULES  dyna:ui (Context, stepper, textField, switch, slider, segmented, combo, spin, drag, navBar, navRail, card, toast) · dyna:sdl (only with --window)
//
// RUN      dynajs --std examples/ui/setup-wizard.js             drives itself headlessly and checks the outcome
//          dynajs --std examples/ui/setup-wizard.js --window    opens it in a window (needs CONFIG_SDL3)

import * as ui from "dyna:ui";

const STEPS = ["Account", "Preferences", "Schedule", "Review"];
const THEMES = ["System", "Light", "Dark"];
const LANGS = ["English", "Deutsch", "Français", "日本語", "हिन्दी"];
const EMAIL = /^[^\s@]+@[^\s@]+\.[^\s@]{2,}$/;
const state = {
    step: 0,
    nav: 0,
    navKind: "",
    tried: false,
    refusedJumps: 0,
    form: { name: "", email: "", notify: true, volume: 40, theme: 0, lang: 0, days: 5, hours: 8 },
    saved: null,
};
const problems = (step, f = state.form) => {
    if (step === 0)
        return [
            !f.name.trim() && "a name is required",
            !EMAIL.test(f.email) && "the email address is not valid",
        ].filter(Boolean);
    if (step === 2) return [f.days * f.hours > 60 && "more than 60 hours a week"].filter(Boolean);
    return [];
};
const firstInvalid = () => STEPS.findIndex((_, i) => problems(i).length > 0);
const rects = {};

function app(u) {
    const rec = (name) => {
        rects[name] = u.lastRect();
    };
    const wide = u.sizeClass === "expanded";
    u.appBar("Set up your workspace", () => {
        u.spacer();
        u.label(u.sizeClass);
    });
    if (u.sizeClass === "compact") {
        state.navKind = "bar";
        state.nav = u.navBar("nav", ["Setup", "Help"], state.nav);
        rec("nav");
        content(u, rec, wide);
    } else
        u.row({ width: "grow", height: "grow", gap: 12, alignY: "start" }, () => {
            state.navKind = "rail";
            state.nav = u.navRail("nav", ["Setup", "Help"], state.nav);
            rec("nav");
            u.column({ width: "grow", gap: 8 }, () => content(u, rec, wide));
        });
}

function content(u, rec, wide) {
    if (state.nav === 1) {
        u.heading("Help");
        u.text("Each step checks its fields before Next moves on. Values survive a resize.", { wrap: true });
        return;
    }
    const clicked = u.stepper("steps", STEPS, state.step);
    rec("steps");
    if (clicked >= 0 && clicked !== state.step) {
        const bad = firstInvalid();
        if (clicked > state.step && bad >= 0 && bad < clicked) {
            state.step = bad;
            state.tried = true;
            state.refusedJumps++;
        } else state.step = clicked;
    }
    u.row({ width: "grow", height: "grow", gap: 16, alignY: "start" }, () => {
        u.column({ width: wide ? { grow: 2 } : "grow", gap: 8 }, () => form(u, rec));
        if (wide) u.column({ width: { grow: 1 }, gap: 6 }, () => summary(u));
    });
}

function form(u, rec) {
    const f = state.form;
    u.heading(STEPS[state.step]);
    if (state.step === 0) {
        u.property("Name", () => {
            f.name = u.textField("##name", f.name, { hint: "Ada Lovelace" });
            rec("name");
        });
        u.property("Email", () => {
            f.email = u.textField("##email", f.email, { hint: "ada@example.com" });
            rec("email");
        });
    } else if (state.step === 1) {
        f.notify = u.switch("Notifications", f.notify);
        rec("notify");
        f.volume = u.slider("Volume", f.volume, 0, 100, { step: 1 });
        rec("volume");
        f.theme = u.segmented("Theme", THEMES, f.theme);
        rec("theme");
        u.property("Language", () => {
            f.lang = u.combo("##lang", LANGS, f.lang);
            rec("lang");
        });
    } else if (state.step === 2) {
        u.property("Days a week", () => {
            f.days = u.spin("##days", f.days, { min: 1, max: 7, step: 1 });
            rec("days");
        });
        u.property("Hours a day", () => {
            f.hours = Math.round(u.drag("##hours", f.hours, { min: 1, max: 16, speed: 0.5 }));
            rec("hours");
        });
        u.progress(Math.min(1, (f.days * f.hours) / 60));
    } else summary(u);
    if (state.tried) for (const p of problems(state.step)) u.text("• " + p, { color: 0xe06c75ff });
    u.row(() => {
        if (state.step > 0 && u.button("Back")) {
            state.step--;
            state.tried = false;
        }
        rec("back");
        u.spacer();
        if (state.step < STEPS.length - 1) {
            if (u.button("Next")) {
                if (problems(state.step).length) state.tried = true;
                else {
                    state.step++;
                    state.tried = false;
                }
            }
            rec("next");
        } else {
            if (u.button("Finish")) {
                state.saved = { ...f, theme: THEMES[f.theme], lang: LANGS[f.lang], weekly: f.days * f.hours };
                u.toast("Saved settings for " + f.name);
            }
            rec("finish");
        }
    });
}

function summary(u) {
    const f = state.form;
    u.card("Summary", () => {
        for (const [k, v] of [
            ["Name", f.name || "—"],
            ["Email", f.email || "—"],
            ["Notifications", f.notify ? "on" : "off"],
            ["Volume", String(f.volume)],
            ["Theme", THEMES[f.theme]],
            ["Language", LANGS[f.lang]],
            ["Week", `${f.days} × ${f.hours} h`],
        ])
            u.row(() => {
                u.text(k, { color: 0x9aa0a6ff });
                u.spacer();
                u.label(v);
            });
    });
}

// ---- headless driver ----
const c = new ui.Context({ width: 390, height: 844, clock: "manual" });
let now = 0;
const frame = () => {
    now += 1 / 60;
    c.setTime(now);
    return c.frame(app);
};
const settle = () => {
    frame();
    frame();
    frame();
};
const mid = (r) => [r.x + r.width / 2, r.y + r.height / 2];
const press = ([x, y]) => {
    c.pointer({ x, y, buttons: 1 });
    frame();
    c.pointer({ x, y, buttons: 0 });
    settle();
};
const drag = ([x0, y0], dx) => {
    c.pointer({ x: x0, y: y0, buttons: 1 });
    frame();
    for (let k = 1; k <= 5; k++) {
        c.pointer({ x: x0 + (dx * k) / 5, y: y0, buttons: 1 });
        frame();
    }
    c.pointer({ x: x0 + dx, y: y0, buttons: 0 });
    settle();
};
const typeInto = (field, text) => {
    press(mid(rects[field]));
    c.type(text);
    settle();
};
const lastStep = () => [rects.steps.x + rects.steps.width - 10, rects.steps.y + rects.steps.height / 2];
const results = {};
if (scriptArgs.includes("--window")) await runWindow();
else {
    settle();
    results["a 390-point phone is compact and gets a bottom navigation bar"] =
        c.sizeClass === "compact" && state.navKind === "bar";

    press(mid(rects.next));
    results["Next on an empty Account is refused, naming both problems"] =
        state.step === 0 && state.tried && problems(0).length === 2;
    press(lastStep());
    results["the stepper cannot jump past a step that does not validate"] =
        state.step === 0 && state.refusedJumps === 1;

    typeInto("name", "Ada Lovelace");
    typeInto("email", "ada@example");
    press(mid(rects.next));
    results["an email without a domain suffix is still refused"] = state.step === 0 && problems(0).length === 1;
    typeInto("email", ".org");
    press(mid(rects.next));
    results["a valid account moves on to Preferences"] = state.step === 1 && state.form.email === "ada@example.org";

    press(mid(rects.notify));
    results["the switch turns notifications off"] = state.form.notify === false;
    const v = rects.volume;
    press([v.x + v.width - 2, v.y + v.height / 2]);
    results["a press at the right end of the slider gives its maximum"] = state.form.volume === 100;
    const th = rects.theme;
    press([th.x + (th.width * 2.5) / 3, th.y + th.height / 2]);
    results["the segmented control picks Dark"] = state.form.theme === 2;
    press(mid(rects.lang));
    const lb = rects.lang;
    press([lb.x + 20, lb.y + lb.height * 3.75]);
    results["the combo picks the fourth language"] = state.form.lang === 3;

    c.resize(800, 390);
    settle();
    results["turned sideways it is medium: a navigation rail, same step, same values"] =
        c.sizeClass === "medium" &&
        state.navKind === "rail" &&
        state.step === 1 &&
        state.form.volume === 100 &&
        state.form.lang === 3;

    press(mid(rects.next));
    const d0 = state.form.days,
        plus = () => [rects.days.x + rects.days.width - 4, rects.days.y + rects.days.height / 2];
    press(plus());
    press(plus());
    press(plus());
    results["three presses on + add three days and stop at 7"] = d0 === 5 && state.form.days === 7;
    drag(mid(rects.hours), 12);
    results["dragging hours 12 points right at speed 0.5 adds 6"] = state.form.hours === 14;
    press(mid(rects.next));
    results["7 × 14 h is over the weekly limit, so Schedule refuses Next"] = state.step === 2 && state.tried;
    drag(mid(rects.hours), -12);
    press(mid(rects.next));
    results["back under the limit it moves on to Review"] = state.step === 3 && state.form.hours === 8;

    c.resize(1280, 800);
    settle();
    results["on a desktop it is expanded, still on Review"] =
        c.sizeClass === "expanded" && state.navKind === "rail" && state.step === 3;
    press(mid(rects.back));
    press(mid(rects.back));
    results["Back walks to Preferences with every value kept"] =
        state.step === 1 && state.form.theme === 2 && state.form.notify === false;
    press(lastStep());
    results["and the stepper may jump forward once everything validates"] =
        state.step === 3 && state.refusedJumps === 1;

    press(mid(rects.finish));
    const s = state.saved;
    results["Finish saves exactly what was entered"] =
        !!s &&
        s.name === "Ada Lovelace" &&
        s.email === "ada@example.org" &&
        s.notify === false &&
        s.volume === 100 &&
        s.theme === "Dark" &&
        s.lang === LANGS[3] &&
        s.days === 7 &&
        s.hours === 8 &&
        s.weekly === s.days * 8;

    results["on a desktop the rail sits left of the form"] =
        rects.nav.x + rects.nav.width <= rects.steps.x && rects.nav.height >= rects.steps.height * 3;
    const nv = rects.nav,
        item = (i) => [nv.x + nv.width / 2, nv.y + 6 + 22 * i + 11];
    for (let i = 0; i < 4 && state.nav !== 1; i++) press(item(i));
    results["the rail switches to Help"] = state.nav === 1;
    const before = c.stats().reusedFrames;
    for (let k = 0; k < 30; k++) {
        c.setTime(now + 10);
        c.frame(app);
    }
    results["an idle wizard reuses its last layout"] = c.stats().reusedFrames - before >= 29;
    c.close();
    check(results);
    print(`setup-wizard: ${Object.keys(results).length} checks passed across compact, medium and expanded`);
}

async function runWindow() {
    let sdl;
    try {
        sdl = await import("dyna:sdl");
    } catch (e) {
        throw new Error("--window needs dyna:sdl (build with CONFIG_SDL3=y)");
    }
    sdl.init(["video"]);
    const win = new sdl.Window("Setup", 900, 640, { resizable: true, highDensity: true });
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
