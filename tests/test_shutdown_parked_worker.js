/* test_shutdown_parked_worker.js -- the worker-side runtime of the row
 * "worker-then-rejection" in test_shutdown_parked.js.
 *
 * This worker's runtime runs and finishes, so its js_std_free_handlers runs
 * while the PARENT runtime is still alive. The rejection-quieting latch that
 * guards the tracker is per THREAD: if it were process-wide, the parent's
 * unhandled rejection -- the row's actual subject -- would be recorded
 * nowhere and the parent would exit 0 instead of 1.
 *
 * The post is DELAYED: a message sent during module evaluation can land
 * before the parent has installed onmessage (the parent then waits forever).
 *
 * Run by: dynajs --std test_shutdown_parked_worker.js (as an os.Worker
 * module). */
import * as os from "os";

setTimeout(() => { os.Worker.parent.postMessage("worker-done"); }, 50);
