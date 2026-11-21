// covers: dyna:postgres.PostgreSQL.queryIter -- next() on a receiver that is not a PgRowStream rejects with that TypeError
// (it rejected with undefined and leaked the reject call's result); no server is contacted.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_postgres_postgre_sql_query_iter_foreign_this");
import { PostgreSQL } from "dyna:postgres";

let fails = 0;
await __kitS.section("next() on a receiver that is not a PgRowStream rejects with TypeError", async () => {
  const pg = new PostgreSQL({ host: "127.0.0.1", port: 1 });
  const it = pg.queryIter("select 1");
  const next = Object.getPrototypeOf(it).next;
  let reason = "unset";
  try { await next.call({}); reason = "resolved"; } catch (e) { reason = e; }
  if (!(reason instanceof TypeError) || !/PgRowStream/.test(reason.message)) { print("FAIL next.call({}) settled with " + String(reason)); fails++; }
  const p = next.call(42);
  if (!(p instanceof Promise)) { print("FAIL next.call(42) did not return a promise"); fails++; }
  try { await p; print("FAIL next.call(42) resolved"); fails++; } catch (e) { if (!(e instanceof TypeError)) { print("FAIL next.call(42) rejected with " + e); fails++; } }
  try { await pg.close(); } catch { }
});
if (fails) throw new Error(fails + " failure(s)");
print("test_postgres_postgre_sql_query_iter_foreign_this: ok");
