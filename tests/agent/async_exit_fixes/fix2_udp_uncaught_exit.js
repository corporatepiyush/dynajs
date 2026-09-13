// FIX2 repro: live UDP socket + uncaught exception -> exit path must not
// abort on the gc_obj_list assert (js_malloc.inc.c JS_FreeRuntime).
// Expected: rc=1, RangeError reported, NO abort/SIGABRT.
import { UDPSocket } from "dyna:net";
const s = new UDPSocket({ port: 0, host: "127.0.0.1" });
s.start({ message: (data, from) => { } });
throw new RangeError("uncaught after UDP start");
