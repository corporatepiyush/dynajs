import { UDPSocket } from "dyna:net";
const s = new UDPSocket({ port: 0, host: "127.0.0.1" });
s.start({ message: (data, from) => { } });
throw new RangeError("uncaught after UDP start");
