import * as os from "os";

setTimeout(() => { os.Worker.parent.postMessage("worker-done"); }, 50);
