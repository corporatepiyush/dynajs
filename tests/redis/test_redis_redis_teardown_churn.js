// flags: --std
// timeout: 300
// covers: dyna:redis.Redis
// Runtime teardown with live Redis clients aimed at a silent peer (the Redis share
// of the old tests/net/test_net_teardown_churn.js; driver: tests/lib/teardown_churn.js).
import { Redis } from "dyna:redis";
import { suite } from "../lib/kit.js";
import { teardownChurn } from "../lib/teardown_churn.js";

await teardownChurn("test_redis_redis_teardown_churn", {
  cyclic: (mp) => {
    const R = new Redis({ port: mp, host: "127.0.0.1" });
    R.on("error", () => { R.command("LATE").catch(() => {}); });
    R.command("PING").catch(() => {});
    return [R];
  },
  plain: (mp) => {
    const R = new Redis({ port: mp, host: "127.0.0.1" });
    R.on("error", () => {});
    R.command("PING").catch(() => {});
  },
}, suite);
