// flags: --std
// timeout: 60
// covers: c:src/dyna-cli.c -- --native-memory-limit takes a plain decimal byte count
// `8e+06` was reported as "must be >= 0 bytes": strtoll stopped at the "e". The parser (cli_parse_bytes) says:
// a leading '-' is "must be >= 0 bytes", any other non-digit start or trailing text is "expects an integer number of
// bytes", a value past 2^64-1 is "out of range"; every message echoes the value. Integers only, by decision.
// Oracle: exit code and stderr of a child per value; accepted values run the script and print its result.
import { Exec, args } from "dyna:sys";
import * as std from "std";

const BIN = args()[0];
let n = 0, fails = 0;
const check = (c, what) => { n++; if (!c) { fails++; print("FAIL: " + what); } };
const NOTINT = /expects an integer number of bytes \(no sign, exponent or suffix\), got '/;
const cases = [
    ["8e+06", NOTINT], ["8E6", NOTINT], ["8.5", NOTINT], ["8000000.0", NOTINT], ["", NOTINT], [" 5", NOTINT], ["+5", NOTINT],
    ["5k", NOTINT], ["5 ", NOTINT], ["0x10", NOTINT], ["NaN", NOTINT], ["Infinity", NOTINT], ["1e400", NOTINT],
    ["-1", /must be >= 0 bytes, got '-1'/], ["-0", /must be >= 0 bytes, got '-0'/], ["-8e+06", /must be >= 0 bytes/],
    ["18446744073709551616", /is out of range \(max 18446744073709551615\), got '18446744073709551616'/],
    ["99999999999999999999999999", /is out of range/],
];
for (const flag of ["--native-memory-limit"]) {
    for (const [v, re] of cases) {
        const c = Exec(BIN, [flag, v, "-e", "print(7)"], { timeoutMs: 20000 });
        check(c.code === 1 && re.test(c.stderr) && c.stderr.includes(flag) && c.stdout === "" && !/>= 0 bytes/.test(c.stderr) === !/^-/.test(v),
            flag + " " + JSON.stringify(v) + ": rc " + c.code + " " + JSON.stringify(c.stderr));
    }
    for (const v of ["0", "18446744073709551615", "8000000", "08000000"]) {
        const c = Exec(BIN, [flag, v, "-e", "print(7)"], { timeoutMs: 20000 });
        check(c.code === 0 && c.stdout.trim() === "7" && c.stderr === "", flag + " " + v + " accepted: rc " + c.code + " " + JSON.stringify(c.stdout + c.stderr));
    }
    const c = Exec(BIN, [flag], { timeoutMs: 20000 });
    check(c.code === 1 && /expects bytes/.test(c.stderr), flag + " without a value: rc " + c.code + " " + JSON.stringify(c.stderr));
}
print("test_cli_native_limit_parse: " + n + " checks, " + fails + " failed");
if (fails) std.exit(1);
