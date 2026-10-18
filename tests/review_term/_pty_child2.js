
import { select } from "dyna:cli";
const r = select("Pick:", ["a", "b"]);
print("R=" + JSON.stringify(r));
