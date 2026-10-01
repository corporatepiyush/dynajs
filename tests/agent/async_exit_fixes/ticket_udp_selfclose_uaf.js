import { UDPSocket } from "dyna:net";
const s = new UDPSocket({ port: 0, host: "127.0.0.1" });
var port = s.port;
s.start({ message: (d, f) => { s.close(); } });
s.send(new Uint8Array([1]), "127.0.0.1", port);
setTimeout(() => { print("survived"); }, 300);
