// Translation units: preprocess every parse target in file-id order and
// hand each file to the first unit that loaded it.
import * as fs from "dyna:file";
import * as std from "std";
import * as os from "os";
import { Tree, Preprocessor, PanicError, predefinedMacros, cleanPath } from "./pp.js";

export function absPath(p) {
  if (p.startsWith("/")) return cleanPath(p);
  let wd = os.getcwd()[0];
  const pwd = std.getenv("PWD");
  if (pwd && pwd.startsWith("/")) {
    try {
      const a = fs.stat(new fs.Path(pwd)), b = fs.stat(new fs.Path(wd));
      if (a.ino === b.ino) wd = pwd;
    } catch (e) {}
  }
  return cleanPath(wd + "/" + p);
}

// files: [{id, path, parsed}], data(id) -> bytes ("" when skipped).
export function unitContext(abs, files, data, defines) {
  const sources = new Map(), allAbs = [], pathID = new Map(), absID = new Map();
  for (const f of files) {
    const full = cleanPath(abs + "/" + f.path);
    allAbs.push(full);
    const d = data(f);
    if (d.length) sources.set(full, d);
    if (!absID.has(full)) absID.set(full, f.id);
    if (f.parsed) pathID.set(full, f.id);
  }
  return { abs, files, sources, pathID, absID, tree: new Tree(abs, allAbs, sources), predef: predefinedMacros(defines) };
}

// Preprocess one unit. Returns null when its root has no bytes, {panic:true}
// when the preprocessor gave up, else the unit with pp and loaded parse targets.
export function preprocessUnit(ctx, id) {
  const f = ctx.files[id - 1];
  const full = cleanPath(ctx.abs + "/" + f.path);
  if (!ctx.sources.has(full)) return null;
  const unit = { root: id, rootAbs: full, pp: null, loaded: [] };
  try {
    unit.pp = new Preprocessor(ctx.tree, full, ctx.predef);
    unit.pp.run();
  } catch (e) {
    if (!(e instanceof PanicError) && !(e instanceof RangeError)) throw e;
    return { root: id, panic: true };
  }
  for (const src of unit.pp.files) {
    const fid = ctx.pathID.get(src.path);
    if (fid !== undefined) unit.loaded.push([src.path, fid]);
  }
  return unit;
}

// Claims in unit order: a unit takes every loaded parse target no earlier
// unit took; a unit whose root was taken already is redundant.
export function claimUnits(results) {
  const claim = new Set();
  let failed = 0, panics = 0;
  for (const u of results) {
    if (u === null) { failed++; continue; }
    if (u.panic) { panics++; failed++; continue; }
    u.claimed = [];
    u.redundant = false;
    for (const [path, fid] of u.loaded) {
      if (!claim.has(path)) { claim.add(path); u.claimed.push(fid); }
      else if (fid === u.root) u.redundant = true;
    }
    if (u.claimed.length === 0 && !u.redundant) failed++;
  }
  return { failed, panics };
}
