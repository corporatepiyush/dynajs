// flags: --std
// Version control — a small Git: content-addressed blobs, trees and commits with Git's exact object format (so
// hashes match real Git), compressed loose objects, a staging index, branches and HEAD, status, log, unified
// diffs, merge-base computation, three-way merges with conflict markers, and fsck.
//
// WHAT IT IS
//   objects      `blob <size>\0<content>`, trees of `<mode> <name>\0<20-byte id>` entries sorted Git's way, and
//                commits with tree / parent / author / committer / message, each stored as a zlib stream (RFC
//                1950: header, DEFLATE, Adler-32) under .vcs/objects/ab/cdef…, so .vcs is a repository real Git reads
//   packs        `gc` moves every loose object into a pack v2 + idx v2 (fanout, sorted ids, CRC32s, offsets, both
//                SHA-1 trailers); a blob or tree whose previous version is already packed becomes an OFS_DELTA
//                (copy the shared prefix and suffix, insert the middle); reads resolve deltas through the idx
//   foreign      where `git` is installed it is the oracle: fsck --strict, rev-list, merge-base, diff-tree and
//                blame must agree with this implementation, before and after gc, and verify-pack must accept the
//                pack — a round trip through our own reader cannot see a mistake we make the same way twice
//   index        `add` stages a file's blob id and mode (as JSON in .vcs/index.json, not Git's binary index); `status` compares working tree, index and HEAD and
//                reports staged, modified, deleted and untracked files
//   history      `commit` writes the tree from the index, `log` walks parents, branches are files under refs/heads
//   diff         a line-level diff between any two commits, rendered as unified hunks with 3 lines of context
//   merge        the merge base is the best common ancestor (BFS over the commit graph); each path is merged
//                three-way: unchanged-on-one-side takes the other, identical changes are taken once, and
//                overlapping line changes become conflict markers; a clean merge commits with two parents,
//                a fast-forward just moves the branch
//   fsck         every object is re-hashed and every reference resolved
//   blame        each line of a file is attributed to the commit that introduced it: a commit's version is diffed
//                (DiffLines) against every parent's blamed version, so a line that arrived through a merge keeps the
//                origin from the branch it was written on; per-commit results are memoised across the DAG
//   graph        reachability and "how many commits ahead" are RoaringBitmaps of interned commit ids, so ahead/behind
//                between two branches is two set differences
//
// MODULES  os (exec git) · dyna:hash (SHA1, Adler32, CRC32) · dyna:compress (deflate) · dyna:matcher (DiffLines) · dyna:file · dyna:encoding (Hex)
//          · dyna:time (formatUnix) · dyna:cli (Table) · dyna:structures (Deque, Interner, RoaringBitmap) · dyna:bytes
//
// RUN      dynajs --std examples/systems/version-control.js     builds a repository with branches, a clean merge and a conflict

import * as os from "os";
import { SHA1, SHA1Hex, Adler32, CRC32 } from "dyna:hash";
import { deflate, inflate } from "dyna:compress";
import { DiffLines } from "dyna:matcher";
import { makeTempDir, makeDir, writeFile, readFile, readBytes, readDir, exists, remove, removeAll } from "dyna:file";
import { HexEncode, HexDecode } from "dyna:encoding";
import { Table } from "dyna:cli";
import { Deque, Interner, RoaringBitmap } from "dyna:structures";
import { concat } from "dyna:bytes";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const enc = new TextEncoder(),
    dec = new TextDecoder();
// Git stores objects as zlib streams (RFC 1950): a 2-byte header, raw DEFLATE, then the Adler-32 of the content
// big-endian. dyna:compress speaks raw DEFLATE, so the frame is built here and checked on the way back.
function zlib(data) {
    const body = deflate(data),
        out = new Uint8Array(body.length + 6);
    out.set([0x78, 0x9c]);
    out.set(body, 2);
    new DataView(out.buffer).setUint32(out.length - 4, Adler32(data));
    return out;
}
function unzlib(bytes) {
    if (bytes.length < 6 || (bytes[0] & 0x0f) !== 8 || ((bytes[0] << 8) | bytes[1]) % 31 !== 0)
        throw new Error("not a zlib stream");
    const data = inflate(bytes.subarray(2, bytes.length - 4)),
        want = new DataView(bytes.buffer, bytes.byteOffset + bytes.length - 4).getUint32(0);
    if (Adler32(data) !== want) throw new Error("zlib checksum mismatch");
    return data;
}

// =============================================================================
// Object store
// =============================================================================
class Repo {
    constructor(root) {
        this.root = root;
        this.git = root.join(".vcs");
        this.clock = 1767225600;
    }
    init() {
        for (const d of ["objects", "refs/heads"]) makeDir(this.git.join(d), { recursive: true });
        writeFile(this.git.join("HEAD"), "ref: refs/heads/main\n");
        writeFile(this.git.join("index.json"), "{}");
        return this;
    }
    hashObject(type, body, write = true) {
        const full = concat([enc.encode(`${type} ${body.length}\0`), body]),
            id = SHA1Hex(full);
        if (write) {
            const dir = this.git.join("objects", id.slice(0, 2));
            if (!exists(dir.join(id.slice(2))) && !this.pack?.offsets.has(id)) {
                makeDir(dir, { recursive: true });
                writeFile(dir.join(id.slice(2)), zlib(full));
            }
        }
        return id;
    }
    readObject(id) {
        const loose = this.git.join("objects", id.slice(0, 2), id.slice(2));
        if (!exists(loose) && this.pack?.offsets.has(id)) return this.packRead(this.pack.offsets.get(id));
        const raw = unzlib(readBytes(loose));
        const nul = raw.indexOf(0),
            [type, size] = dec.decode(raw.subarray(0, nul)).split(" ");
        const body = raw.subarray(nul + 1);
        if (body.length !== Number(size)) throw new Error(`object ${id}: size mismatch`);
        return { type, body };
    }
    writeTree(entries) {
        // entries: { name, mode, id }; Git sorts trees as if named "name/"
        const sorted = [...entries].sort((a, b) => {
            const ka = a.mode === "40000" ? a.name + "/" : a.name,
                kb = b.mode === "40000" ? b.name + "/" : b.name;
            return ka < kb ? -1 : ka > kb ? 1 : 0;
        });
        return this.hashObject(
            "tree",
            concat(sorted.flatMap((e) => [enc.encode(`${e.mode} ${e.name}\0`), HexDecode(e.id)])),
        );
    }
    readTree(id, prefix = "", out = new Map()) {
        const { body } = this.readObject(id);
        for (let off = 0; off < body.length;) {
            const sp = body.indexOf(32, off),
                nul = body.indexOf(0, sp);
            const mode = dec.decode(body.subarray(off, sp)),
                name = dec.decode(body.subarray(sp + 1, nul)),
                oid = HexEncode(body.subarray(nul + 1, nul + 21));
            if (mode === "40000") this.readTree(oid, `${prefix}${name}/`, out);
            else out.set(prefix + name, { id: oid, mode });
            off = nul + 21;
        }
        return out;
    }
    treeFromIndex(index) {
        // build nested trees bottom-up
        const build = (prefix) => {
            const entries = [],
                subdirs = new Set();
            for (const [path, e] of Object.entries(index)) {
                if (!path.startsWith(prefix)) continue;
                const rest = path.slice(prefix.length),
                    slash = rest.indexOf("/");
                if (slash < 0) entries.push({ name: rest, mode: e.mode, id: e.id });
                else subdirs.add(rest.slice(0, slash));
            }
            for (const d of subdirs) entries.push({ name: d, mode: "40000", id: build(`${prefix}${d}/`) });
            return this.writeTree(entries);
        };
        return build("");
    }
    // ----- refs -----
    head() {
        const h = readFile(this.git.join("HEAD")).trim();
        return h.startsWith("ref: ") ? { branch: h.slice(16), id: this.ref(h.slice(5)) } : { branch: null, id: h };
    }
    ref(name) {
        const p = this.git.join(name);
        return exists(p) ? readFile(p).trim() : null;
    }
    setRef(name, id) {
        writeFile(this.git.join(name), id + "\n");
    }
    branch(name, at = this.head().id) {
        this.setRef(`refs/heads/${name}`, at);
    }
    // ----- working tree -----
    files(dir = this.root, prefix = "") {
        const out = new Map();
        for (const e of readDir(dir)) {
            if (e.name === ".vcs") continue;
            if (e.isDir) for (const [k, v] of this.files(dir.join(e.name), `${prefix}${e.name}/`)) out.set(k, v);
            else out.set(prefix + e.name, readBytes(dir.join(e.name)));
        }
        return out;
    }
    index() {
        return JSON.parse(readFile(this.git.join("index.json")));
    }
    add(...paths) {
        const idx = this.index(),
            wt = this.files();
        for (const p of paths) {
            if (wt.has(p)) idx[p] = { id: this.hashObject("blob", wt.get(p)), mode: "100644" };
            else delete idx[p];
        }
        writeFile(this.git.join("index.json"), JSON.stringify(idx));
    }
    addAll() {
        const wt = this.files(),
            idx = this.index();
        this.add(...new Set([...wt.keys(), ...Object.keys(idx)]));
    }
    commit(message, author = "Ada Lovelace <ada@example.com>", extraParents = []) {
        const tree = this.treeFromIndex(this.index()),
            h = this.head(),
            parents = [h.id, ...extraParents].filter(Boolean);
        this.clock += 3600;
        const body = [
            `tree ${tree}`,
            ...parents.map((p) => `parent ${p}`),
            `author ${author} ${this.clock} +0000`,
            `committer ${author} ${this.clock} +0000`,
            "",
            message,
            "",
        ].join("\n");
        const id = this.hashObject("commit", enc.encode(body));
        h.branch ? this.setRef(`refs/heads/${h.branch}`, id) : writeFile(this.git.join("HEAD"), id + "\n");
        return id;
    }
    readCommit(id) {
        const text = dec.decode(this.readObject(id).body),
            [head, ...msg] = text.split("\n\n");
        const c = { id, parents: [], message: msg.join("\n\n").trim() };
        for (const line of head.split("\n")) {
            const [k, ...v] = line.split(" ");
            if (k === "parent") c.parents.push(v[0]);
            else if (k === "tree") c.tree = v[0];
            else if (k === "author") {
                c.author = v.slice(0, -2).join(" ");
                c.time = Number(v.at(-2));
            }
        }
        return c;
    }
    checkout(branch) {
        const id = this.ref(`refs/heads/${branch}`),
            tree = this.readTree(this.readCommit(id).tree);
        for (const p of this.files().keys()) if (!tree.has(p)) remove(this.root.join(p));
        const idx = {};
        for (const [p, e] of tree) {
            const path = this.root.join(p);
            makeDir(path.dirname, { recursive: true });
            writeFile(path, this.readObject(e.id).body);
            idx[p] = e;
        }
        writeFile(this.git.join("index.json"), JSON.stringify(idx));
        writeFile(this.git.join("HEAD"), `ref: refs/heads/${branch}\n`);
    }
    status() {
        const idx = this.index(),
            wt = this.files(),
            headTree = this.head().id ? this.readTree(this.readCommit(this.head().id).tree) : new Map(),
            out = [];
        for (const [p, e] of Object.entries(idx)) {
            const h = headTree.get(p);
            if (!h) out.push(["new file", p]);
            else if (h.id !== e.id) out.push(["staged", p]);
        }
        out.push(
            ...headTree
                .keys()
                .filter((p) => !idx[p])
                .map((p) => ["deleted (staged)", p]),
        );
        for (const [p, bytes] of wt) {
            if (!idx[p]) out.push(["untracked", p]);
            else if (this.hashObject("blob", bytes, false) !== idx[p].id) out.push(["modified", p]);
        }
        out.push(
            ...Object.keys(idx)
                .filter((p) => !wt.has(p))
                .map((p) => ["deleted", p]),
        );
        return out;
    }
    log(from = this.head().id) {
        const out = [],
            seen = new Set(),
            q = [from];
        while (q.length) {
            const id = q.shift();
            if (!id || seen.has(id)) continue;
            seen.add(id);
            const c = this.readCommit(id);
            out.push(c);
            q.push(...c.parents);
        }
        return out.sort((a, b) => b.time - a.time);
    }
    mergeBase(a, b) {
        const ancestors = (id) => {
            const d = new Map(),
                q = new Deque();
            q.pushBack([id, 0]);
            while (true) {
                const x = q.popFront();
                if (!x) break;
                const [c, n] = x;
                if (d.has(c)) continue;
                d.set(c, n);
                for (const p of this.readCommit(c).parents) q.pushBack([p, n + 1]);
            }
            return d;
        };
        const da = ancestors(a),
            db = ancestors(b);
        return (
            da
                .keys()
                .filter((c) => db.has(c))
                .minBy((c) => da.get(c) + db.get(c)) ?? null
        );
    }
    diff(fromId, toId) {
        const A = this.readTree(this.readCommit(fromId).tree),
            B = this.readTree(this.readCommit(toId).tree),
            out = [];
        for (const p of [...new Set([...A.keys(), ...B.keys()])].sort()) {
            const a = A.get(p),
                b = B.get(p);
            if (a?.id === b?.id) continue;
            const ta = a ? dec.decode(this.readObject(a.id).body) : "",
                tb = b ? dec.decode(this.readObject(b.id).body) : "";
            out.push(unified(p, ta, tb, !a, !b));
        }
        return out.join("");
    }
    merge(branch) {
        const ours = this.head().id,
            theirs = this.ref(`refs/heads/${branch}`),
            base = this.mergeBase(ours, theirs);
        if (base === theirs) return { result: "already up to date" };
        if (base === ours) {
            this.setRef(`refs/heads/${this.head().branch}`, theirs);
            this.checkout(this.head().branch);
            return { result: "fast-forward", to: theirs };
        }
        const B = this.readTree(this.readCommit(base).tree),
            O = this.readTree(this.readCommit(ours).tree),
            T = this.readTree(this.readCommit(theirs).tree);
        const idx = {},
            conflicts = [];
        const text = (e) => (e ? dec.decode(this.readObject(e.id).body) : null);
        for (const p of new Set([...B.keys(), ...O.keys(), ...T.keys()])) {
            const b = B.get(p),
                o = O.get(p),
                t = T.get(p);
            if (o?.id === t?.id) {
                if (o) idx[p] = o;
                continue;
            }
            if (b?.id === o?.id) {
                if (t) idx[p] = t;
                continue;
            }
            if (b?.id === t?.id) {
                if (o) idx[p] = o;
                continue;
            }
            const merged = merge3(text(b) ?? "", text(o) ?? "", text(t) ?? "", this.head().branch, branch);
            const path = this.root.join(p);
            makeDir(path.dirname, { recursive: true });
            writeFile(path, merged.text);
            if (merged.conflicts) conflicts.push(p);
            idx[p] = { id: this.hashObject("blob", enc.encode(merged.text)), mode: "100644" };
        }
        writeFile(this.git.join("index.json"), JSON.stringify(idx));
        for (const [p, e] of Object.entries(idx))
            if (!conflicts.includes(p)) {
                const path = this.root.join(p);
                makeDir(path.dirname, { recursive: true });
                writeFile(path, this.readObject(e.id).body);
            }
        if (conflicts.length) return { result: "conflict", conflicts, base };
        return { result: "merged", commit: this.commit(`Merge branch '${branch}'`, undefined, [theirs]), base };
    }
    fileAt(commitId, path) {
        const entry = this.readTree(this.readCommit(commitId).tree).get(path);
        return entry ? dec.decode(this.readObject(entry.id).body) : null;
    }
    blame(path, at = this.head().id, memo = new Map()) {
        // [commit id, line] for every line of path at `at`
        if (memo.has(at)) return memo.get(at);
        const text = this.fileAt(at, path) ?? "",
            lines = splitLines(text),
            origin = new Array(lines.length).fill(null);
        for (const parent of this.readCommit(at).parents) {
            // a line any parent already had keeps that parent's origin
            const before = this.blame(path, parent, memo),
                parentText = before.map(([, l]) => l).join("\n") + (before.length ? "\n" : "");
            let i = 0,
                j = 0; // i: line in this version, j: line in the parent
            for (const h of DiffLines(parentText, text)) {
                const n = splitLines(h.text).length;
                if (h.op === 0) {
                    for (let k = 0; k < n; k++) origin[i + k] ??= before[j + k][0];
                    i += n;
                    j += n;
                } else if (h.op === -1) j += n;
                else i += n;
            }
        }
        const out = lines.map((line, k) => [origin[k] ?? at, line]);
        memo.set(at, out);
        return out;
    }
    reachable(from) {
        // every ancestor of `from`, as a bitmap of interned ids
        this.commitIds ??= new Interner();
        const seen = new RoaringBitmap(),
            q = new Deque();
        q.pushBack(from);
        while (q.length) {
            const id = q.popFront();
            if (!seen.tryAdd(this.commitIds.intern(id))) continue;
            for (const p of this.readCommit(id).parents) q.pushBack(p);
        }
        return seen;
    }
    aheadBehind(a, b) {
        const ra = this.reachable(this.ref(`refs/heads/${a}`)),
            rb = this.reachable(this.ref(`refs/heads/${b}`));
        return { ahead: ra.andNotCardinality(rb), behind: rb.andNotCardinality(ra) };
    }
    // ----- packs -----
    // gc() moves every loose object into objects/pack/pack-<sha>.{pack,idx} (pack v2, idx v2). A blob or tree whose
    // previous version under the same path is already in the pack is stored as an OFS_DELTA against it: copy
    // instructions for the shared prefix and suffix, an insert for the middle.
    gc() {
        const loose = readDir(this.git.join("objects"))
            .filter((d) => d.name.length === 2)
            .flatMap((d) => readDir(this.git.join("objects", d.name)).map((f) => d.name + f.name));
        const objs = new Map(loose.map((id) => [id, this.readObject(id)]));
        // order: commits oldest first, then for each commit its trees and blobs, so a delta base precedes its user
        const order = [],
            placed = new Set(),
            baseOf = new Map(),
            lastAt = new Map();
        const place = (id, path) => {
            if (placed.has(id) || !objs.has(id)) return;
            placed.add(id);
            const prev = lastAt.get(path);
            if (prev && prev !== id && objs.get(prev).type === objs.get(id).type) baseOf.set(id, prev);
            lastAt.set(path, id);
            order.push(id);
        };
        const walkTree = (id, prefix) => {
            place(id, prefix + "/");
            const { body } = objs.get(id);
            for (let off = 0; off < body.length;) {
                const sp = body.indexOf(32, off),
                    nul = body.indexOf(0, sp);
                const mode = dec.decode(body.subarray(off, sp)),
                    name = dec.decode(body.subarray(sp + 1, nul)),
                    oid = HexEncode(body.subarray(nul + 1, nul + 21));
                if (mode === "40000") walkTree(oid, prefix + name + "/");
                else place(oid, prefix + name);
                off = nul + 21;
            }
        };
        const commits = [...objs]
            .filter(([, o]) => o.type === "commit")
            .map(([id]) => this.readCommit(id))
            .sort((a, b) => a.time - b.time);
        for (const c of commits) {
            place(c.id, "commit");
            baseOf.delete(c.id); // commits are small and unique: stored whole
            walkTree(c.tree, "");
        }
        for (const id of objs.keys()) place(id, "?" + id);
        const TYPES = { commit: 1, tree: 2, blob: 3, tag: 4 },
            chunks = [],
            offsets = new Map(),
            crcs = new Map();
        const head = new Uint8Array(12);
        head.set(enc.encode("PACK"));
        new DataView(head.buffer).setUint32(4, 2);
        new DataView(head.buffer).setUint32(8, order.length);
        chunks.push(head);
        let at = 12,
            deltas = 0;
        for (const id of order) {
            const { type, body } = objs.get(id),
                base = baseOf.get(id);
            const payload = base ? makeDelta(objs.get(base).body, body) : body;
            const useDelta = base && payload.length < body.length * 0.8;
            const bytes = [];
            let size = useDelta ? payload.length : body.length,
                b = ((useDelta ? 6 : TYPES[type]) << 4) | (size & 15);
            size = Math.floor(size / 16);
            while (size) {
                bytes.push(b | 0x80);
                b = size & 127;
                size = Math.floor(size / 128);
            }
            bytes.push(b);
            if (useDelta) {
                // the distance back to the base, in Git's offset encoding (each continuation adds one before shifting)
                let rel = at - offsets.get(base);
                const ofs = [rel & 127];
                while ((rel = Math.floor(rel / 128))) ofs.unshift(0x80 | (--rel & 127));
                bytes.push(...ofs);
                deltas++;
            }
            const entry = concat([Uint8Array.from(bytes), zlib(useDelta ? payload : body)]);
            offsets.set(id, at);
            crcs.set(id, CRC32(entry));
            chunks.push(entry);
            at += entry.length;
        }
        const packBody = concat(chunks),
            packSum = SHA1(packBody),
            pack = concat([packBody, packSum]);
        // idx v2: magic, version, fanout[256], sorted ids, CRC32s, 32-bit offsets, pack checksum, idx checksum
        const ids = [...offsets.keys()].sort(),
            n = ids.length,
            idx = new Uint8Array(8 + 1024 + n * 28 + 20),
            dv = new DataView(idx.buffer);
        idx.set([0xff, 0x74, 0x4f, 0x63]);
        dv.setUint32(4, 2);
        for (let k = 0, i = 0; k < 256; k++) {
            while (i < n && parseInt(ids[i].slice(0, 2), 16) <= k) i++;
            dv.setUint32(8 + 4 * k, i);
        }
        ids.forEach((id, i) => {
            idx.set(HexDecode(id), 1032 + 20 * i);
            dv.setUint32(1032 + 20 * n + 4 * i, crcs.get(id));
            dv.setUint32(1032 + 24 * n + 4 * i, offsets.get(id));
        });
        idx.set(packSum, 1032 + 28 * n);
        const idxFile = concat([idx, SHA1(idx)]);
        const name = `pack-${HexEncode(packSum)}`;
        makeDir(this.git.join("objects/pack"), { recursive: true });
        writeFile(this.git.join("objects/pack", name + ".pack"), pack);
        writeFile(this.git.join("objects/pack", name + ".idx"), idxFile);
        for (const id of loose) remove(this.git.join("objects", id.slice(0, 2), id.slice(2)));
        for (const d of readDir(this.git.join("objects")))
            if (d.name.length === 2 && !readDir(this.git.join("objects", d.name)).length)
                remove(this.git.join("objects", d.name));
        this.openPack();
        return { objects: n, deltas, packBytes: pack.length, looseBytes: loose.length };
    }
    openPack() {
        // reads the idx the way Git does: fanout to narrow, then the sorted id table
        const dir = this.git.join("objects/pack");
        const name = exists(dir) ? readDir(dir).find((e) => e.name.endsWith(".idx"))?.name : null;
        if (!name) return (this.pack = null);
        const idx = readBytes(dir.join(name)),
            dv = new DataView(idx.buffer, idx.byteOffset),
            n = dv.getUint32(8 + 4 * 255),
            bytes = readBytes(dir.join(name.replace(/\.idx$/, ".pack"))),
            offsets = new Map();
        if (dv.getUint32(0) !== 0xff744f63 || dv.getUint32(4) !== 2) throw new Error("not an idx v2 file");
        if (HexEncode(SHA1(idx.subarray(0, idx.length - 20))) !== HexEncode(idx.subarray(idx.length - 20)))
            throw new Error("idx checksum mismatch");
        for (let i = 0; i < n; i++)
            offsets.set(HexEncode(idx.subarray(1032 + 20 * i, 1052 + 20 * i)), dv.getUint32(1032 + 24 * n + 4 * i));
        const ends = [...offsets.values()].sort((a, b) => a - b);
        this.pack = { bytes, offsets, end: new Map(ends.map((o, i) => [o, ends[i + 1] ?? bytes.length - 20])) };
    }
    packRead(off) {
        const { bytes, end } = this.pack;
        let p = off,
            c = bytes[p++];
        const type = (c >> 4) & 7;
        let size = c & 15,
            shift = 4;
        while (c & 0x80) {
            c = bytes[p++];
            size += (c & 127) * 2 ** shift;
            shift += 7;
        }
        let base = -1;
        if (type === 6) {
            c = bytes[p++];
            let rel = c & 127;
            while (c & 0x80) {
                c = bytes[p++];
                rel = (rel + 1) * 128 + (c & 127);
            }
            base = off - rel;
        }
        const data = unzlib(bytes.subarray(p, end.get(off)));
        if (data.length !== size) throw new Error(`pack entry at ${off}: size mismatch`);
        if (type !== 6) return { type: ["", "commit", "tree", "blob", "tag"][type], body: data };
        const b = this.packRead(base);
        return { type: b.type, body: applyDelta(b.body, data) };
    }
    fsck() {
        const objects = readDir(this.git.join("objects"))
            .filter((d) => d.name.length === 2)
            .flatMap((d) => readDir(this.git.join("objects", d.name)).map((f) => [d.name, f.name]));
        const problems = objects
            .filter(([d, f]) => {
                try {
                    return SHA1Hex(unzlib(readBytes(this.git.join("objects", d, f)))) !== d + f;
                } catch {
                    return true;
                }
            })
            .map(([d, f]) => `object ${d}${f} is corrupt`);
        for (const b of readDir(this.git.join("refs/heads"))) {
            try {
                this.readCommit(this.ref(`refs/heads/${b.name}`));
            } catch {
                problems.push(`ref ${b.name} points to a missing commit`);
            }
        }
        return { objects: objects.length, problems };
    }
}
// Git's delta format: two varint sizes, then instructions: 0x80|flags copies (offset, size) from the base, 1..127
// inserts that many literal bytes. This encoder copies the common prefix and suffix and inserts the rest.
const varint = (n) => {
    const out = [];
    do {
        let b = n & 127;
        n = Math.floor(n / 128);
        if (n) b |= 0x80;
        out.push(b);
    } while (n);
    return out;
};
function makeDelta(base, target) {
    let pre = 0;
    while (pre < base.length && pre < target.length && base[pre] === target[pre]) pre++;
    let suf = 0;
    while (
        suf < base.length - pre &&
        suf < target.length - pre &&
        base[base.length - 1 - suf] === target[target.length - 1 - suf]
    )
        suf++;
    const out = [...varint(base.length), ...varint(target.length)];
    const copy = (offset, size) => {
        for (let done = 0; done < size;) {
            const n = Math.min(size - done, 0xffff),
                o = offset + done,
                args = [];
            let op = 0x80;
            for (let k = 0; k < 4; k++) if ((o >>> (8 * k)) & 255) ((op |= 1 << k), args.push((o >>> (8 * k)) & 255));
            for (let k = 0; k < 2; k++) if ((n >>> (8 * k)) & 255) ((op |= 16 << k), args.push((n >>> (8 * k)) & 255));
            out.push(op, ...args);
            done += n;
        }
    };
    if (pre) copy(0, pre);
    for (let k = pre; k < target.length - suf; k += 127) {
        const chunk = target.subarray(k, Math.min(k + 127, target.length - suf));
        out.push(chunk.length, ...chunk);
    }
    if (suf) copy(base.length - suf, suf);
    return Uint8Array.from(out);
}
function applyDelta(base, delta) {
    let p = 0;
    const read = () => {
        let n = 0,
            shift = 0,
            c;
        do {
            c = delta[p++];
            n += (c & 127) * 2 ** shift;
            shift += 7;
        } while (c & 0x80);
        return n;
    };
    if (read() !== base.length) throw new Error("delta base size mismatch");
    const out = new Uint8Array(read());
    let q = 0;
    while (p < delta.length) {
        const op = delta[p++];
        if (op & 0x80) {
            let off = 0,
                size = 0;
            for (let k = 0; k < 4; k++) if (op & (1 << k)) off |= delta[p++] << (8 * k);
            for (let k = 0; k < 3; k++) if (op & (16 << k)) size |= delta[p++] << (8 * k);
            size ||= 0x10000;
            if (off + size > base.length || q + size > out.length) throw new Error("delta copy out of range");
            out.set(base.subarray(off, off + size), q);
            q += size;
        } else if (op) {
            if (q + op > out.length) throw new Error("delta insert out of range");
            out.set(delta.subarray(p, p + op), q);
            p += op;
            q += op;
        } else throw new Error("delta opcode 0 is reserved");
    }
    if (q !== out.length) throw new Error("delta result size mismatch");
    return out;
}
function unified(path, a, b, added, removed) {
    const lines = DiffLines(a, b).flatMap((h) =>
        h.text
            .split("\n")
            .slice(0, h.text.endsWith("\n") ? -1 : undefined)
            .map((l) => [h.op, l]),
    );
    let out = `--- ${added ? "/dev/null" : "a/" + path}\n+++ ${removed ? "/dev/null" : "b/" + path}\n`;
    const changed = lines.map((l, i) => (l[0] !== 0 ? i : -1)).filter((i) => i >= 0);
    let i = 0;
    while (i < changed.length) {
        let start = Math.max(0, changed[i] - 3),
            end = changed[i];
        while (i < changed.length && changed[i] - end <= 6) {
            end = changed[i];
            i++;
        }
        end = Math.min(lines.length - 1, end + 3);
        const block = lines.slice(start, end + 1);
        const aStart = lines.slice(0, start).filter((l) => l[0] <= 0).length + 1,
            bStart = lines.slice(0, start).filter((l) => l[0] >= 0).length + 1;
        out +=
            `@@ -${aStart},${block.filter((l) => l[0] <= 0).length} +${bStart},${block.filter((l) => l[0] >= 0).length} @@\n` +
            block.map(([op, l]) => `${op < 0 ? "-" : op > 0 ? "+" : " "}${l}\n`).join("");
    }
    return out;
}
const splitLines = (t) => (t === "" ? [] : (t.endsWith("\n") ? t.slice(0, -1) : t).split("\n"));
function edits(base, other) {
    // base ranges [bs, be) replaced by lines
    const out = [];
    let bi = 0,
        cur = null;
    for (const h of DiffLines(base, other)) {
        const ls = splitLines(h.text);
        if (h.op === 0) {
            if (cur) {
                out.push(cur);
                cur = null;
            }
            bi += ls.length;
        } else {
            cur ??= { bs: bi, be: bi, lines: [] };
            if (h.op < 0) {
                bi += ls.length;
                cur.be = bi;
            } else cur.lines.push(...ls);
        }
    }
    if (cur) out.push(cur);
    return out;
}
function merge3(base, ours, theirs, oursName, theirsName) {
    // diff3: overlapping change regions conflict
    const B = splitLines(base),
        eo = edits(base, ours),
        et = edits(base, theirs),
        out = [];
    let i = 0,
        j = 0,
        pos = 0,
        conflicts = 0;
    while (i < eo.length || j < et.length) {
        const start = Math.min(eo[i]?.bs ?? Infinity, et[j]?.bs ?? Infinity);
        out.push(...B.slice(pos, start));
        let end = start,
            oi = i,
            ti = j,
            grew = true;
        while (grew) {
            grew = false;
            while (oi < eo.length && eo[oi].bs <= end) {
                end = Math.max(end, eo[oi].be);
                oi++;
                grew = true;
            }
            while (ti < et.length && et[ti].bs <= end) {
                end = Math.max(end, et[ti].be);
                ti++;
                grew = true;
            }
        }
        const side = (E, from, to) => {
            const res = [];
            let p = start;
            for (let k = from; k < to; k++) {
                res.push(...B.slice(p, E[k].bs), ...E[k].lines);
                p = E[k].be;
            }
            res.push(...B.slice(p, end));
            return res;
        };
        const o = side(eo, i, oi),
            t = side(et, j, ti);
        if (oi === i) out.push(...t);
        else if (ti === j || o.join("\n") === t.join("\n")) out.push(...o);
        else {
            conflicts++;
            out.push(`<<<<<<< ${oursName}`, ...o, "=======", ...t, `>>>>>>> ${theirsName}`);
        }
        pos = end;
        i = oi;
        j = ti;
    }
    out.push(...B.slice(pos));
    return { text: out.join("\n") + (ours.endsWith("\n") || theirs.endsWith("\n") ? "\n" : ""), conflicts };
}

// =============================================================================
// Scenario
// =============================================================================
const results = {};
const work = makeTempDir("vcs"),
    repo = new Repo(work).init();
results['blob ids match Git (`git hash-object` of "hello\\n")'] =
    repo.hashObject("blob", enc.encode("hello\n"), false) === "ce013625030ba8dba906f756967f9e9ca394464a";
results["the empty tree id matches Git's well-known constant"] =
    repo.writeTree([]) === "4b825dc642cb6eb9a060e54bf8d69288fbee4904";
const w = (p, text) => {
    const path = work.join(p);
    makeDir(path.dirname, { recursive: true });
    writeFile(path, text);
};
w("README.md", "# Inventory service\n\nTracks stock levels.\n");
w(
    "src/stock.js",
    [
        "export function reserve(stock, sku, qty) {",
        "  const have = stock.get(sku) ?? 0;",
        "  if (have < qty) return false;",
        "  stock.set(sku, have - qty);",
        "  return true;",
        "}",
        "",
    ].join("\n"),
);
w(
    "src/report.js",
    ["export function report(stock) {", "  return [...stock].map(([k, v]) => k + ': ' + v).join('\\n');", "}", ""].join(
        "\n",
    ),
);
results["three untracked files before the first add"] = repo.status().filter(([s]) => s === "untracked").length === 3;
repo.addAll();
const c1 = repo.commit("Initial import");
repo.branch("audit-log");
repo.branch("hotfix");
// main: change README
w("README.md", "# Inventory service\n\nTracks stock levels and reservations.\n");
repo.addAll();
const c2 = repo.commit("Docs: mention reservations");
// feature/audit: add logging to reserve() and a new file
repo.checkout("audit-log");
w(
    "src/stock.js",
    [
        "export function reserve(stock, sku, qty, log = []) {",
        "  const have = stock.get(sku) ?? 0;",
        "  if (have < qty) return false;",
        "  stock.set(sku, have - qty);",
        "  log.push({ sku, qty });",
        "  return true;",
        "}",
        "",
    ].join("\n"),
);
w("src/audit.js", "export const audit = (log) => log.length;\n");
repo.addAll();
const f1 = repo.commit("Audit log for reservations", "Grace Hopper <grace@example.com>");
results["status is clean after commit"] = repo.status().length === 0;
repo.checkout("main");
results["checkout swaps the working tree"] =
    !exists(work.join("src/audit.js")) && readFile(work.join("README.md")).includes("reservations");
const mergeAudit = repo.merge("audit-log");
results["a clean three-way merge combines both branches"] =
    mergeAudit.result === "merged" &&
    mergeAudit.base === c1 &&
    readFile(work.join("src/stock.js")).includes("log.push") &&
    exists(work.join("src/audit.js"));
results["the merge commit has two parents"] = repo.readCommit(mergeAudit.commit).parents.length === 2;
// hotfix: conflicting edit of the same line in stock.js
repo.checkout("hotfix");
w(
    "src/stock.js",
    [
        "export function reserve(stock, sku, qty) {",
        "  const have = stock.get(sku) ?? 0;",
        "  if (qty <= 0 || have < qty) return false;",
        "  stock.set(sku, have - qty);",
        "  return true;",
        "}",
        "",
    ].join("\n"),
);
repo.addAll();
repo.commit("Reject non-positive quantities", "Linus T <lt@example.com>");
repo.checkout("main");
w(
    "src/stock.js",
    readFile(work.join("src/stock.js")).replace(
        "if (have < qty) return false;",
        "if (have < qty) throw new Error('insufficient stock');",
    ),
);
repo.addAll();
repo.commit("Throw on insufficient stock");
const conflict = repo.merge("hotfix");
const conflictText = readFile(work.join("src/stock.js"));
results["overlapping edits produce conflict markers naming both sides"] =
    conflict.result === "conflict" &&
    conflict.conflicts[0] === "src/stock.js" &&
    /<<<<<<< main[\s\S]*throw new Error[\s\S]*=======[\s\S]*qty <= 0[\s\S]*>>>>>>> hotfix/.test(conflictText);
w(
    "src/stock.js",
    conflictText.replace(
        /<<<<<<< main\n.*\n=======\n.*\n>>>>>>> hotfix/,
        "  if (qty <= 0) return false;\n  if (have < qty) throw new Error('insufficient stock');",
    ),
);
repo.addAll();
const resolved = repo.commit("Merge branch 'hotfix'", undefined, [repo.ref("refs/heads/hotfix")]);
repo.branch("release");
repo.checkout("release");
w("CHANGELOG", "1.1: reservations, audit, validation\n");
repo.addAll();
const r1 = repo.commit("Changelog");
repo.checkout("main");
const ff = repo.merge("release");
results["merging a descendant fast-forwards"] = ff.result === "fast-forward" && repo.head().id === r1;
const blame = repo.blame("src/stock.js");
const originOf = (needle) => blame.find(([, l]) => l.includes(needle))?.[0];
results["blame follows merges back to the commit that wrote each line"] =
    originOf("log.push") === f1 &&
    originOf("export function reserve") === f1 &&
    originOf("stock.set") === c1 &&
    originOf("insufficient stock") !== c1 &&
    blame.every(([id]) => id);
repo.branch("experiment");
repo.checkout("experiment");
w("NOTES", "try a faster index\n");
repo.addAll();
repo.commit("Notes");
w("NOTES", "try a faster index\nmeasure first\n");
repo.addAll();
repo.commit("More notes");
repo.checkout("main");
const ab = repo.aheadBehind("experiment", "main");
results[`ahead/behind by bitmap differences: experiment is ${ab.ahead} ahead, ${ab.behind} behind main`] =
    ab.ahead === 2 && ab.behind === 0;
const patch = repo.diff(c1, repo.head().id);
results["unified diff with hunk headers and /dev/null for new files"] =
    /^--- a\/src\/stock\.js\n\+\+\+ b\/src\/stock\.js\n@@ -1,\d+ \+1,\d+ @@/m.test(patch) &&
    /^--- \/dev\/null\n\+\+\+ b\/CHANGELOG/m.test(patch);
const history = repo.log();
results[`log walks ${history.length} commits through both merges`] =
    history.length === 8 && history[0].id === r1 && history.some((c) => c.id === resolved && c.parents.length === 2);
// Real Git as the foreign reader: it must accept every object, walk the same history, pick the same merge base,
// see the same changed paths and blame each line on the same commit. Skipped, and said so, where git is absent.
const scratch = makeTempDir("vcs-git");
function git(...args) {
    const outPath = scratch.join("out").toString(),
        fd = os.open(outPath, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644),
        null2 = os.open("/dev/null", os.O_WRONLY);
    let rc = -1;
    try {
        rc = os.exec(["git", "--git-dir", work.join(".vcs").toString(), ...args], {
            usePath: true,
            stdout: fd,
            stderr: null2,
            env: {
                PATH: "/usr/bin:/bin:/usr/local/bin:/opt/homebrew/bin",
                GIT_CONFIG_NOSYSTEM: "1",
                HOME: scratch.toString(),
            },
        });
    } finally {
        os.close(fd);
        os.close(null2);
    }
    return { rc, out: readFile(scratch.join("out")).trim() };
}
const haveGit = (() => {
    try {
        return git("--version").rc === 0;
    } catch {
        return false;
    }
})();
const gitChecks = (label) => {
    const fsck = git("fsck", "--strict", "--no-dangling"),
        revList = git("rev-list", "main").out.split("\n"),
        ours = repo.log().map((c) => c.id),
        mb = git("merge-base", "main", "experiment").out,
        tree = git("diff-tree", "-r", "--name-only", c1, repo.head().id).out.split("\n"),
        blameLines = git("blame", "-s", "-l", "main", "--", "src/stock.js").out.split("\n"),
        oursBlame = repo.blame("src/stock.js");
    const changed = [...repo.readTree(repo.readCommit(repo.head().id).tree)]
        .filter(([p, e]) => repo.readTree(repo.readCommit(c1).tree).get(p)?.id !== e.id)
        .map(([p]) => p)
        .sort();
    const verdict = {
        fsck: fsck.rc === 0,
        revList: revList.join() === ours.join(),
        mergeBase: mb === repo.mergeBase(repo.ref("refs/heads/main"), repo.ref("refs/heads/experiment")),
        diffTree: tree.join() === changed.join(),
        blame:
            blameLines.length === oursBlame.length &&
            blameLines.every((l, i) => l.replace(/^\^/, "").startsWith(oursBlame[i][0].slice(0, 39))),
    };
    if (!Object.values(verdict).every(Boolean)) print(`  git disagrees (${label}): ${JSON.stringify(verdict)}`);
    return Object.values(verdict).every(Boolean);
};
if (!haveGit) print("  SKIP       git is not installed: the foreign-reader checks did not run");
results["real git accepts the loose objects and agrees on log, merge-base, changed paths and blame"] =
    !haveGit || gitChecks("loose");
const looseCount = repo.fsck().objects;
const gc = repo.gc();
const afterGc = {
    log: repo
        .log()
        .map((c) => c.id)
        .join(),
    blame: JSON.stringify(repo.blame("src/stock.js")),
    files: [...repo.readTree(repo.readCommit(repo.head().id).tree)].map(([p, e]) => p + e.id).join(),
};
results[`gc packs ${gc.objects} objects (${gc.deltas} as deltas) into one pack + idx; history reads back identically`] =
    gc.objects === looseCount &&
    gc.deltas > 0 &&
    readDir(work.join(".vcs/objects")).every((d) => d.name === "pack") &&
    afterGc.log === history.map((c) => c.id).join() &&
    afterGc.blame === JSON.stringify(blame);
const verifyPack = haveGit
    ? git(
          "verify-pack",
          "-v",
          work
              .join(
                  ".vcs/objects/pack",
                  readDir(work.join(".vcs/objects/pack")).find((e) => e.name.endsWith(".idx")).name,
              )
              .toString(),
      )
    : null;
results["real git verifies the pack and idx (CRCs, offsets, delta chains) and still agrees on everything"] =
    !haveGit || (verifyPack.rc === 0 && /chain length = 1:/.test(verifyPack.out) && gitChecks("packed"));
// a pack whose delta claims more bytes than its base holds must be refused, not read past
let refusedDelta = "";
try {
    applyDelta(enc.encode("abc"), Uint8Array.from([3, 5, 0x91, 0, 5]));
} catch (e) {
    refusedDelta = e.message;
}
results["a delta that copies past its base is refused"] = refusedDelta === "delta copy out of range";
// new objects after gc go loose again, beside the pack
// "src.md" beside the directory "src": Git sorts a tree as if a directory's name ended in "/", and '.' < '/', so
// the file comes first; a plain name sort puts the directory first and git fsck --strict rejects the tree
w("NOTES.md", "post-gc note\n");
w("src.md", "# Sources\n");
repo.addAll();
const postGc = repo.commit("After gc");
results["a commit after gc writes loose objects that read through the pack-aware path"] =
    repo.readCommit(postGc).parents[0] === r1 && repo.fileAt(postGc, "README.md").includes("reservations");
results["real git accepts the tree that holds both src.md and src/ (Git's tree order)"] =
    !haveGit || git("fsck", "--strict", "--no-dangling").rc === 0;
removeAll(scratch);
const fsck1 = repo.fsck();
const anyObj = readDir(work.join(".vcs/objects")).find((d) => d.name.length === 2),
    victim = work.join(".vcs/objects", anyObj.name, readDir(work.join(".vcs/objects", anyObj.name))[0].name);
writeFile(victim, zlib(enc.encode("blob 3\0bad")));
const fsck2 = repo.fsck();
results[`fsck verified ${fsck1.objects} loose objects beside the pack, then caught the corrupted one`] =
    fsck1.problems.length === 0 && fsck2.problems.length === 1;

print("version-control");
print(
    table([
        ["commit", "author", "parents", "message"],
        ...history.map((c) => [c.id.slice(0, 10), c.author.replace(/ <.*/, ""), String(c.parents.length), c.message]),
    ]),
);
print(
    patch
        .split("\n")
        .slice(0, 14)
        .map((l) => "  " + l)
        .join("\n"),
);
print(
    blame
        .map(([id, line]) => `  ${id.slice(0, 8)} ${repo.readCommit(id).author.replace(/ <.*/, "").padEnd(14)} ${line}`)
        .join("\n"),
);
print(
    `  objects    ${looseCount} loose -> 1 pack of ${gc.packBytes} bytes (${gc.deltas} deltas); branches ${readDir(
        work.join(".vcs/refs/heads"),
    )
        .map((e) => e.name)
        .join(", ")}`,
);
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
