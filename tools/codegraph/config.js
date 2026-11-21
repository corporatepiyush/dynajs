// Project defines: the CODEGRAPH_DEFINES list, or the scanned dyna set.
import * as fs from "dyna:file";
import * as std from "std";
import { readBytesString } from "./pp.js";

const VALUE_FLAGS = new Set(["--module", "--limit", "--json", "--csv", "--dump", "--save", "--save-ast", "--load-ast",
  "--defects-baseline", "--update-baseline", "--jobs"]);
const words = (s) => (s || "").trim().split(/\s+/).filter(Boolean);
const isDir = (p) => { try { return fs.stat(new fs.Path(p)).isDir; } catch (e) { return false; } };

export function scanRoot(argv) {
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a.startsWith("-")) { if (VALUE_FLAGS.has(a)) i++; continue; }
    return isDir(a) ? a : ".";
  }
  return ".";
}

function scanModuleMacros(dir, seen) {
  let ents;
  try { ents = fs.readDir(new fs.Path(dir)); } catch (e) { return; }
  for (const e of ents) {
    const p = dir === "." ? e.name : dir + "/" + e.name;
    if (e.isDir) {
      if (e.name.startsWith(".") || e.name === "third_party" || e.name === "test262" || e.name === "node_modules") continue;
      scanModuleMacros(p, seen);
    } else if (!e.isSymlink || !isDir(p)) {
      const dot = e.name.lastIndexOf(".");
      const ext = dot < 0 ? "" : e.name.slice(dot);
      if (ext !== ".c" && ext !== ".h" && ext !== ".inc") continue;
      let src;
      try { src = readBytesString(p); } catch (x) { continue; }
      if (!src.includes("CONFIG_NATIVE_MODULE_")) continue;
      for (const m of src.matchAll(/\bCONFIG_NATIVE_MODULE_[A-Z0-9_]+\b/g)) seen.add(m[0]);
    }
  }
}

export function projectDefines(argv) {
  const env = std.getenv("CODEGRAPH_DEFINES");
  if (env && env.trim() !== "") {
    return words(env).map((f) => { const i = f.indexOf("="); return i < 0 ? [f, "1"] : [f.slice(0, i), f.slice(i + 1)]; }).filter(([n]) => n !== "");
  }
  const seen = new Set(["CONFIG_NATIVE_MODULES", "CONFIG_TLS", "CONFIG_SQLITE", "CONFIG_ZSTD", "CONFIG_SDL3"]);
  scanModuleMacros(scanRoot(argv), seen);
  for (const f of words(std.getenv("CODEGRAPH_EXTRA_DEFINES"))) { const n = f.split("=")[0]; if (n) seen.add(n); }
  return [...seen].sort().map((n) => [n, "1"]);
}
