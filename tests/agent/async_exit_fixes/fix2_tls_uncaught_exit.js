// FIX2 TLS variant: live TLS server with handlers + uncaught exception.
// Expected: rc=1 with the RangeError, NO abort.
import { TCPServer } from "dyna:net";
const srv = new TCPServer({
  port: 0, host: "127.0.0.1",
  tls: { cert: "scratch/cert.pem", key: "scratch/key.pem" }
});
srv.start({
  connect: (c) => { },
  data: (c, data) => { },
  close: (c) => { }
});
throw new RangeError("uncaught after TLS start");
