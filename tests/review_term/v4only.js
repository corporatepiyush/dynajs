import { Command } from "dyna:cli";
const r = new Command("t").parse();
print("args=" + r.arguments.length);
