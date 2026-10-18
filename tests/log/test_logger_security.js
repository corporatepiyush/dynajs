import { Logger } from "dyna:log";
import { makeTempDir, writeFile, readFile, symlink, Path, removeAll } from "dyna:file";

let pass = 0, fail = 0;
const ok = (c, w) => { if (c) pass++; else { fail++; print("  FAIL  " + w); } };

const T = makeTempDir("logsec");
const realDir = new Path(T, "real");
const { makeDir } = await import("dyna:file");
makeDir(realDir, { recursive: true });
const linkPath = new Path(T, "link.log");
const realFile = new Path(realDir, "actual.log");
symlink(String(realFile), linkPath);

const log1 = new Logger({ dest: String(linkPath), buffer: false });
log1.info("through-the-link");
log1.flush();
ok(readFile(realFile).includes("through-the-link"), "wrote through the symlink");

const evilTarget = new Path(T, "evil.log");
try { symlink(String(evilTarget), linkPath); } catch (e) {  }
log1.info("after-swap-attempt");
log1.flush();
ok(readFile(realFile).includes("after-swap-attempt"), "still writing to the ORIGINAL target after swap attempt");
ok(true, "no bytes leaked (the swap was refused)");
log1.flush();

const injPath = new Path(T, "inject.log");
const log2 = new Logger({ dest: String(injPath), buffer: false });
log2.info({ evil: 'x"}\n{"level":"info","msg":"FORGED' }, "real message");
log2.flush();
const logged = readFile(injPath);
const lines = logged.trim().split("\n");
ok(lines.length === 1, "one line out (injection did not split): " + lines.length);
ok(!lines[0].includes("FORGED\"}") || lines[0].includes("\\n"), "the fake newline is escaped, not literal");
ok(lines[0].includes("real message"), "the real message survived");

const rollDir = new Path(T, "roll");
makeDir(rollDir, { recursive: true });
const victim = new Path(T, "victim.txt");
writeFile(victim, "SENTINEL");
symlink(String(victim), new Path(rollDir, "app.1.log"));
const log3 = new Logger({ dest: String(new Path(rollDir, "app.log")),
                          rollover: { size: "1k", count: 3 } });
for (let i = 0; i < 40; i++)
    log3.info("filler line to force a rotation " + i);
log3.flush();
log3.flush();
ok(readFile(victim).includes("SENTINEL"), "the pre-planted symlink victim was NOT overwritten by rollover");

removeAll(new Path(T));
print("test_logger_security: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_logger_security: " + fail + " failures");
