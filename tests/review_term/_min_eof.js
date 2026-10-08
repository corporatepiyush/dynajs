import { keypress, prompt } from "dyna:cli";
const k = keypress();
print("K=" + JSON.stringify(k));
const s = prompt("P:", { default: "D" });
print("S=" + JSON.stringify(s));
