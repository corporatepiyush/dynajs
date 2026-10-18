// A client whose peer hung up no longer holds the event loop: this script
// keeps the handle, never calls close(), and must still exit on its own. A
// regression is a HANG, which the runner reports as a timeout.
import { TCPServer } from "dyna:net";

const srv = new TCPServer({ port: 0 });
srv.start({ connect(conn) { conn.close(); srv.close(); } });
let closes = 0;
globalThis.keptHandle = TCPServer.connect({ host: "127.0.0.1", port: srv.port }, {
    close() {
        closes++;
        print("test_net_client_peer_closed_exit: close handler ran (" + closes + "); 1 passed, 0 failed");
    },
});
