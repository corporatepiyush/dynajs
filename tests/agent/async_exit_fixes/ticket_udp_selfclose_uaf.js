// repro (ticketed, NOT fixed in this lane): a UDP
// message handler that closes its own socket frees h_message (dispose) while
// udp_on_message is still executing that handler — use-after-free, visible
// under ASan. Doctrine check on non-ASan builds: the loop must survive and
// exit cleanly either way.
import { UDPSocket } from "dyna:net";
const s = new UDPSocket({ port: 0, host: "127.0.0.1" });
var port = s.port;
s.start({ message: (d, f) => { s.close(); } });
s.send(new Uint8Array([1]), "127.0.0.1", port);
setTimeout(() => { print("survived"); }, 300);
