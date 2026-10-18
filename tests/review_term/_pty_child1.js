
import { keypress, prompt } from "dyna:cli";
import * as std from "std";
std.out.puts("READY\n");
std.out.flush();
const k = keypress();
print("K=" + JSON.stringify(k && k.name));
// after keypress the tty must be back to canonical+echo; the observable proof
// is the next LINE read: with echo+canonical on, the tty driver line-buffers
// and the whole line arrives at once.
const s = prompt("P:");
print("S=" + JSON.stringify(s));
