import { Command } from "dyna:cli";
let e = null;
try { new Command("t").parse(new Array(65537).fill("x")); } catch (x) { e = x; }
print("caught=" + (e && e.constructor.name));
