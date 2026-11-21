import { SSHClient } from "dyna:ssh";
import * as std from "std";
import { FakeSSH } from "./fake_server.js";
import { tmpDir, keygen, write } from "./ssh_util.js";

const mode = scriptArgs[1];
const dir = tmpDir("teardown");
const user = keygen(dir + "/user");
const f = new FakeSSH(mode === "connect-pending-throw" ? { greeting: "", onGreeted: () => {} } : { onExec: () => {} });
write(dir + "/kh", f.knownHostsLine());
const opts = { host: "127.0.0.1", port: f.port, username: "u", privateKey: user.priv, knownHosts: dir + "/kh" };
if (mode === "connect-pending-throw") {
    SSHClient.connect(opts).catch(() => {});
    await new Promise((r) => setTimeout(r, 50));
    throw new Error("teardown-boom");
}
const c = await SSHClient.connect(opts);
const x = await c.exec("hang");
x.stdout.getReader().read();
x.exit.then(() => {});
console.log("exec-in-flight");
if (mode === "exec-pending-exit") std.exit(3);
throw new Error("teardown-boom");
