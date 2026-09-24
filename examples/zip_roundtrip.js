// examples/zip_roundtrip.js — zip acceptance: per-entry fields, by-index access, iteration, extract-all.
import { ZipPack, ZipList, ZipReadAt, ZipExtractAll, deflate, inflate } from "dyna:compress";
const enc = new TextEncoder(), dec = new TextDecoder();
const data = enc.encode("hello ".repeat(200));
const raw = deflate(data);
print("deflate", raw.length, dec.decode(inflate(raw)).length === dec.decode(data).length);
const zip = ZipPack([
  { name: "a.txt", data, mtime: 1700000000, mode: 0o644, comment: "first" },
  { name: "b.txt", data: enc.encode("x"), method: "store", comment: "second" },
]);
for (const e of ZipList(zip)) print("entry", e.name, e.method, e.comment);
print("by-index", ZipReadAt(zip, 1).name);
print("extracted", ZipExtractAll(zip).length);
