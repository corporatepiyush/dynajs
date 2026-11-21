// flags: --std
// timeout: 300
// covers: dyna:postgres.PostgreSQL
// Runtime teardown with live PostgreSQL clients aimed at a silent peer (the
// PostgreSQL share of the old tests/net/test_net_teardown_churn.js; driver:
// tests/lib/teardown_churn.js).
import { PostgreSQL } from "dyna:postgres";
import { suite } from "../lib/kit.js";
import { teardownChurn } from "../lib/teardown_churn.js";

await teardownChurn("test_postgres_postgre_sql_teardown_churn", {
  cyclic: (mp) => {
    const P = new PostgreSQL({ port: mp, host: "127.0.0.1", user: "u" });
    P.on("error", () => { P.query("SELECT late").catch(() => {}); });
    P.query("SELECT 1").catch(() => {});
    return [P];
  },
  plain: (mp) => {
    const P = new PostgreSQL({ port: mp, host: "127.0.0.1", user: "u" });
    P.on("error", () => {});
    P.query("SELECT 1").catch(() => {});
  },
}, suite);
