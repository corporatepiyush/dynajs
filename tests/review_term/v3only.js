import { Command } from "dyna:cli";
scriptArgs = new Array(70000).fill("y");
let e = null;
try { new Command("t").parse(); } catch (x) { e = x; }
print("caught=" + (e && e.constructor.name));
