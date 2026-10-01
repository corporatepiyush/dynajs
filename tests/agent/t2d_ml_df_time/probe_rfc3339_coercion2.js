import { formatRFC3339 } from "dyna:time";
function t(l, f) {
    try { console.log(l, "->", JSON.stringify(f())); }
    catch (e) { console.log(l, "-> THROWS", e.constructor.name, String(e.message)); }
}
t("bag nsec abc", () => formatRFC3339(0, { nsec: "abc" }));
t("bag nsec null", () => formatRFC3339(0, { nsec: null }));
t("bag nsec {}", () => formatRFC3339(0, { nsec: {} }));
t("bag nsec undefined", () => formatRFC3339(0, { nsec: undefined }));
t("bag nsec 0/0", () => formatRFC3339(0, { nsec: 0 / 0 }));
t("bag nsec 2.5", () => formatRFC3339(0, { nsec: 2.5 }));
t("legacy abc", () => formatRFC3339(0, "abc"));
t("legacy 2.5", () => formatRFC3339(0, 2.5));
t("legacy -0.5", () => formatRFC3339(0, -0.5));
t("legacy 0n", () => formatRFC3339(0, 0n));
t("bag off abc", () => formatRFC3339(0, { offsetMinutes: "abc" }));
t("bag off 0/0", () => formatRFC3339(0, { offsetMinutes: 0 / 0 }));
t("legacy(0, 7, 1)", () => formatRFC3339(0, 7, 1));
t("legacy(0, 7, 0)", () => formatRFC3339(0, 7, 0));
