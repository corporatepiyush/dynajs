// flags: --std
// timeout: 300
// covers: dyna:net.TCPServer
// Runtime teardown with live TCP connects and UDP sockets (the net share of the
// old tests/net/test_net_teardown_churn.js; the driver is tests/lib/teardown_churn.js).
import { TCPServer, UDPSocket } from "dyna:net";
import { suite } from "../lib/kit.js";
import { teardownChurn } from "../lib/teardown_churn.js";

await teardownChurn("test_net_tcp_server_teardown_churn", {
  cyclic: () => {
    const U = new UDPSocket({ port: 0, host: "127.0.0.1" });
    U.start({ message: (d) => { U.send(d, "127.0.0.1", 1); } });
    let C;
    C = TCPServer.connect({ host: "10.255.255.1", port: 54321 }, {
      connect: () => { if (C) C.close(); },
    });
    return [U, C];
  },
  plain: () => {
    const U = new UDPSocket({ port: 0, host: "127.0.0.1" });
    U.start({ message: () => {} });
    TCPServer.connect({ host: "10.255.255.1", port: 54321 }, { connect: () => {} });
  },
}, suite);
