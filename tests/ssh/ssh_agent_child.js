// Child process for the agent teardown cases (run under LeakSanitizer by test_ssh_ssh_agent.js when BIN is the sanitizer build).
// close-busy: an agent with keys, raw connections with requests (signatures and a lock hash, which run on the worker pool) written but not
// yet answered, an SSHClient mid-authentication against a scripted agent that never answers the sign request, then everything closed and
// the event loop left to drain (no std.exit).
// exit-live: the same peers left open and std.exit(0) with the agent still listening (the shutdown sweep must free it and unlink the socket).
import { SSHAgent, SSHServer, SSHClient } from "dyna:ssh";
import { Ed25519Generate } from "dyna:crypto";
import { TCPServer } from "dyna:net";
import { W } from "./fake_server.js";
import { AG, frame, cat, edBlob, addEd, scriptedAgent, idsAnswer, sockDir } from "./agent_util.js";
import { authLine } from "./fake_client.js";
import { tmpDir, keygen, write, pubBlob } from "./ssh_util.js";

const [mode, path] = scriptArgs.slice(1);
const dir = tmpDir("agchild");
const host = keygen(dir + "/host");
const kp = Ed25519Generate();
const g = SSHAgent.listen({ path, keys: [{ privateKey: keygen(dir + "/u").priv, comment: "c" }] });
const conns = [];
for (let i = 0; i < 4; i++) {
    conns.push(TCPServer.connect({ path }, { connect: (c) => { if (!c) return; c.write(cat(frame(addEd(Ed25519Generate())), frame(Uint8Array.of(AG.REQ_IDS)), frame(new W().u8(AG.SIGN).str(edBlob(kp.publicKey)).str("x").u32(0).done()), frame(new W().u8(AG.LOCK).str("p").done()))); } }));
}
const hung = scriptedAgent(sockDir(dir) + "/hung.sock", (m) => (m[0] === AG.REQ_IDS ? idsAnswer([[edBlob(kp.publicKey), "k"]]) : null));
const s = SSHServer.listen({ hostKey: host.priv, authorizedKeys: authLine(kp.publicKey) + "\n", onSession: (x) => x.exit(0) });
write(dir + "/kh", "[127.0.0.1]:" + s.port + " " + pubBlob(host.pub) + "\n");
const pending = SSHClient.connect({ host: "127.0.0.1", port: s.port, username: "u", agent: sockDir(dir) + "/hung.sock", knownHosts: dir + "/kh" }).catch((e) => e);
await new Promise((r) => setTimeout(r, 300));
print("agent-ran", JSON.stringify(g.stats));
if (mode === "exit-live") std.exit(0);
g.close();
for (const c of conns) c.close();
s.close();
const e = await pending;
hung.close();
print("client:", String(e && e.message));
