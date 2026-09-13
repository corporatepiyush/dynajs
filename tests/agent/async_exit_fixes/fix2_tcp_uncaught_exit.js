// FIX2 TCP variant: live TCP server with handlers + uncaught exception.
// Expected: rc=1 with the RangeError, NO abort.
import { TCPServer } from "dyna:net";
const srv = new TCPServer({ port: 0, host: "127.0.0.1" });
srv.start({
  connect: (c) => { c.write("hi"); },
  data: (c, data) => { },
  close: (c) => { }
});
throw new RangeError("uncaught after TCP start");
