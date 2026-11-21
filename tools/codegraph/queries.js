// Every query and metric runner, by the oracle's function name.
import { Q } from "./q1.js";
import "./q2.js";
import { D } from "./qdyn.js";
import { M } from "./qmetrics.js";

export const RUNNERS = { ...Q, ...D, ...M };
