// The app the Android APK runs by default: tools/android/build-apk.sh run tools/android/demo.js
// A touch-first screen (app bar, chips, wrapping cards, a list with long-press menu, settings,
// a sheet, a drawer, a bottom nav bar, a FAB). Its log (build-apk.sh log) names every state change,
// so a driver tapping through adb can check what each tap did.
import * as ui from "dyna:ui";
import * as sdl from "dyna:sdl";

sdl.init(["video"]);
const win = new sdl.Window("dynajs", 1080, 2400, { resizable: true });
const r = new sdl.Renderer(win);
const c = new ui.Context({ target: r, touch: "auto" });

const state = { nav: 0, filters: [true, false, false, true], wifi: true, bt: false, vol: 0.6, sel: -1, rating: 3, toasts: 0, sheet: 0, name: "", calls: 0 };
const items = Array.from({ length: 200 }, (_, i) => `Contact ${i}`);

function app(u) {
    u.appBar(["Home", "Contacts", "Settings"][state.nav], () => { if (u.button("Menu")) u.openDrawer("nav"); });
    if (state.nav === 0) {
        u.wrap(() => ["New", "Unread", "Starred", "Shared"].forEach((f, i) => { state.filters[i] = u.chip(f, state.filters[i]); }));
        u.wrap(() => {
            for (let i = 0; i < 6; i++)
                u.card("Card " + i, { width: 160 }, () => {
                    u.text("Body text that wraps inside the card.");
                    if (u.button("Open##" + i)) { state.sheet++; u.openSheet("detail"); }
                });
        });
        state.rating = u.rating("Rate", state.rating);
    } else if (state.nav === 1) {
        state.sel = u.list("Contacts", items, state.sel, { rows: 12 });
        u.contextMenu("cm", () => { if (u.menuItem("Call")) { state.calls++; u.toast("Calling..."); } });
    } else {
        u.group("Connectivity", () => { state.wifi = u.switch("Wi-Fi", state.wifi); state.bt = u.switch("Bluetooth", state.bt); });
        state.vol = Math.round(u.slider("Volume", state.vol, 0, 1) * 100) / 100;
        state.name = u.textField("Name", state.name);
    }
    u.sheet("detail", () => { u.heading("Details"); if (u.button("Close")) u.closePopup(); });
    u.drawer("nav", () => ["Home", "Contacts", "Settings"].forEach((n, i) => { if (u.selectable(n, state.nav === i)) { state.nav = i; u.closePopup(); } }));
    state.nav = u.navBar("tabs", ["Home", "Contacts", "Settings"], state.nav);
    if (u.fab("##add")) { state.toasts++; u.toast("New item"); }
}

let running = true, frames = 0, last = "", size = "";
while (running) {
    for (const e of sdl.pollEvents()) if (e.type === "quit") running = false;
    if (c.frame(app)) frames++;
    const sz = `${c.width}x${c.height} pt ${c.pixelWidth}x${c.pixelHeight} px scale ${c.scale} ${c.sizeClass} insets ${c.insets.map((v) => v.toFixed(2)).join(",")}`;
    if (sz !== size) { print("screen", sz); size = sz; }
    const s = JSON.stringify(state);
    if (s !== last) { print("state", s, "frames", frames); last = s; }
    c.idle(1);
}
c.close();
r.close();
win.close();
