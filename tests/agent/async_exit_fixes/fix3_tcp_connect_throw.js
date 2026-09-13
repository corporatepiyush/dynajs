// FIX3 TCP variant: connect-handler throw observed via -d (stderr diagnostic
// once per kind + "net swallowed handler throws" line in the -d dump).
// Exits rc=0: the doctrine keeps the loop alive; srv.close() lets it drain.
import { TCPServer } from "dyna:net";
const srv = new TCPServer({ port: 0, host: "127.0.0.1" });
srv.start({ connect: (c) => { throw new Error("connect-boom"); } });
TCPServer.connect({ host: "127.0.0.1", port: srv.port }, (c, err) => { });
setTimeout(() => { srv.close(); }, 300);
