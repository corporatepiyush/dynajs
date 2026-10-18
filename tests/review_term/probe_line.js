// flags: --std
import { prompt, confirm } from "dyna:cli";
const mode = scriptArgs[1] || "prompt";
if (mode === "prompt") {
    let r1 = "OK1", r2 = "OK2";
    try { r1 = "R:" + prompt("P1: "); } catch (e) { r1 = "E:" + e.constructor.name; }
    try { r2 = "R:" + prompt("P2: "); } catch (e) { r2 = "E:" + e.constructor.name;
        try { r2 += "/R:" + prompt("P3: "); } catch (e2) { r2 += "/E:" + e2.constructor.name; } }
    print("A1=" + JSON.stringify(r1));
    print("A2=" + JSON.stringify(r2));
} else if (mode === "confirm") {
    let r1 = "OK1", r2 = "OK2";
    try { r1 = "R:" + confirm("C1: "); } catch (e) { r1 = "E:" + e.constructor.name;
        try { r1 += "/R:" + confirm("C2: "); } catch (e2) { r1 += "/E:" + e2.constructor.name; } }
    try { r2 = "R:" + confirm("C3: "); } catch (e2) { r2 = "E:" + e2.constructor.name; }
    print("A1=" + JSON.stringify(r1));
    print("A2=" + JSON.stringify(r2));
} else if (mode === "cap-exact") {
    let r = "?";
    try { r = "R-len:" + prompt("Q: ").length; } catch (e) { r = "E:" + e.constructor.name; }
    print("A1=" + JSON.stringify(r));
    let r2 = "?";
    try { r2 = "R:" + prompt("Q2: "); } catch (e) { r2 = "E:" + e.constructor.name; }
    print("A2=" + JSON.stringify(r2));
} else if (mode === "cap-crlf") {
    let r = "?";
    try { r = "R-len:" + prompt("Q: ").length; } catch (e) { r = "E:" + e.constructor.name; }
    print("A1=" + JSON.stringify(r));
}
