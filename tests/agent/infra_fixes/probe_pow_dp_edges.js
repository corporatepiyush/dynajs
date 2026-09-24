/* probe: dyna:decimal pow-exponent and round/toFixed dp validation edges
   (inputs for the gen_decimal.py pin set; INFRA 6) */
import { Decimal } from "dyna:decimal";
const D = (s) => new Decimal(s);
const t = (w, f) => { try { print(w, "ok:" + String(f())); } catch (e) { print(w, e.constructor.name + ":" + e.message); } };
t("pow 0.5  ", () => D(2).pow(0.5));
t("pow -0.5 ", () => D(2).pow(-0.5));
t("pow NaN  ", () => D(2).pow(NaN));
t("pow +Inf ", () => D(2).pow(Infinity));
t("pow -Inf ", () => D(2).pow(-Infinity));
t("pow 1e300", () => D(2).pow(1e300));
t("pow -1e300", () => D(2).pow(-1e300));
t("pow 10001", () => D(2).pow(10001));
t("pow -10001", () => D(2).pow(-10001));
t("pow 10000", () => D(2).pow(10000).toString().length);
t("pow 2^53+1", () => D(2).pow(9007199254740993));
t("round dp 0.5  ", () => D("1.5").round(0.5));
t("round dp NaN  ", () => D("1.5").round(NaN));
t("round dp +Inf ", () => D("1.5").round(Infinity));
t("round dp -Inf ", () => D("1.5").round(-Infinity));
t("round dp 1e300", () => D("1.5").round(1e300));
t("round dp -1e300", () => D("1.5").round(-1e300));
t("toFixed dp 0.5  ", () => D("1.5").toFixed(0.5));
t("toFixed dp NaN  ", () => D("1.5").toFixed(NaN));
t("toFixed dp +Inf ", () => D("1.5").toFixed(Infinity));
t("toFixed dp -Inf ", () => D("1.5").toFixed(-Infinity));
