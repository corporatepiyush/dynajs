// flags: --std
// line_driver.js -- prompt()/confirm() 1 MiB cap + stream desync probes (stdin is a pipe).
import { run as drive, bytes, fill, check, done } from "./drive.js";

const MIB = 1 << 20;
const run = (mode, payload) => drive("probe_line.js", mode, payload, (l) => l.includes("A1=") || l.includes("A2=") || l === "<TIMEOUT>");
const grab = (lines, tag) => { for (const l of lines) { const i = l.indexOf(tag); if (i >= 0) return l.slice(i); } return "<missing>"; };
const J = JSON.stringify;

let got = run("prompt", bytes(fill("x", 2 * MIB), "\nsecond\n"));
let a1 = grab(got, "A1="), a2 = grab(got, "A2=");
check("P21", "2 MiB line: RangeError, next prompt still reads 'second' (no desync)",
    a1 === 'A1="E:RangeError"' && a2 === 'A2="R:second"', `A1=${J(a1)} A2=${J(a2)} raw=${J(got)}`);

got = run("confirm", bytes(fill("y", 2 * MIB), "\nyes\n"));
a1 = grab(got, "A1="); a2 = grab(got, "A2=");
check("P22", "confirm: 2 MiB line RangeError then aligned 'yes' -> true",
    a1 === 'A1="E:RangeError/R:true"' && a2 === 'A2="R:false"', `A1=${J(a1)} A2=${J(a2)} raw=${J(got)}`);

got = run("cap-exact", bytes(fill("a", MIB), "\nsecond\n"));
a1 = grab(got, "A1="); a2 = grab(got, "A2=");
check("P23", "exactly 1 MiB + LF accepted at the boundary, stream aligned",
    a1 === `A1="R-len:${MIB}"` && a2 === 'A2="R:second"', `A1=${J(a1)} A2=${J(a2)} raw=${J(got)}`);

got = run("cap-crlf", bytes(fill("a", MIB), "\r\n"));
a1 = grab(got, "A1=");
check("P24", "1 MiB + CRLF accepted as 1 MiB of content (CR dropped)",
    a1 === `A1="R-len:${MIB}"`, `A1=${J(a1)} (if E:RangeError the CR counts against the cap -- boundary inconsistency with P23)`);
done("line_driver");
