// recpreload.js -- node-only preload used at EXPECTATION-BAKE time (never shipped
// in final probe execution on dynajs). Sets the recorder flag.
globalThis.__REC__ = 1;
