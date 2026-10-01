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
