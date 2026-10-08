import { PostgreSQL, Redis } from "dyna:net";

let _seq = 0;

export class PgPool {
    constructor(opts = {}) {
        this._opts = {};
        for (const k of ["host","port","user","password","database","path","tls","ca","stmtCacheMax","maxPending","connectTimeoutMs","queryTimeoutMs"])
            if (opts[k] !== undefined) this._opts[k] = opts[k];
        this.size = Math.max(1, opts.size | 0 || 4);
        this.idleMs = opts.idleMs ?? 30000;
        this.acquireTimeoutMs = Math.max(1, opts.acquireTimeoutMs | 0 || 5000);
        this._free = [];
        this._used = new Set();
        this._waiters = [];
        this._closed = false;
        this._idleTimers = new Map();
    }

    get stats() {
        return {
            total: this._free.length + this._used.size,
            free: this._free.length,
            used: this._used.size,
            waiting: this._waiters.length,
            closed: this._closed,
        };
    }

    _spawn() {
        const c = new PostgreSQL(this._opts);
        c._poolId = ++_seq;
        return c;
    }

    _isBad(c) {
        try {
            const st = c.transactionStatus;
            if (st === "E" || st === "T") return true;
        } catch (_) {}
        return false;
    }

    _discard(c) {
        try { c.close(); } catch (_) {}
        this._idleTimers.delete(c);
    }

    _scheduleIdle(c) {
        if (this.idleMs <= 0 || this._closed) return;
        const t = setTimeout(() => {
            const idx = this._free.indexOf(c);
            if (idx >= 0) {
                this._free.splice(idx, 1);
                this._discard(c);
            }
            this._idleTimers.delete(c);
        }, this.idleMs);
        this._idleTimers.set(c, t);
    }

    _cancelIdle(c) {
        const t = this._idleTimers.get(c);
        if (t !== undefined) {
            clearTimeout(t);
            this._idleTimers.delete(c);
        }
    }

    _retire(c) {
        this._used.delete(c);
        const fi = this._free.indexOf(c);
        if (fi >= 0) this._free.splice(fi, 1);
        this._cancelIdle(c);
        this._discard(c);
        if (this._waiters.length && !this._closed) {
            const w = this._waiters.shift();
            try {
                const nc = this._spawn();
                w.resolve(nc);
            } catch (err) {
                w.reject(err);
            }
        }
    }

    async acquire() {
        if (this._closed) throw new RangeError("PgPool: closed");

        if (this._free.length) {
            const c = this._free.pop();
            this._cancelIdle(c);
            this._used.add(c);
            return c;
        }

        if (this._free.length + this._used.size < this.size) {
            const c = this._spawn();
            this._used.add(c);
            return c;
        }

        return await new Promise((resolve, reject) => {
            const w = {
                resolve: (c) => { clearTimeout(timer); this._used.add(c); resolve(c); },
                reject: (e) => { clearTimeout(timer); reject(e); },
            };
            const timer = setTimeout(() => {
                const i = this._waiters.indexOf(w);
                if (i >= 0) this._waiters.splice(i, 1);
                w.reject(new RangeError(`PgPool: acquire timed out after ${this.acquireTimeoutMs}ms`));
            }, this.acquireTimeoutMs);
            this._waiters.push(w);
        });
    }

    release(c) {
        if (!this._used.has(c)) {
            if (this._free.includes(c)) return;
            this._discard(c);
            return;
        }
        this._used.delete(c);
        const fi = this._free.indexOf(c);
        if (fi >= 0) this._free.splice(fi, 1);
        this._cancelIdle(c);

        if (this._closed) {
            this._discard(c);
            return;
        }

        if (this._isBad(c)) {
            this._discard(c);
            if (this._waiters.length) {
                const w = this._waiters.shift();
                const nc = this._spawn();
                w.resolve(nc);
            }
            return;
        }

        if (this._waiters.length) {
            const w = this._waiters.shift();
            w.resolve(c);
        } else {
            this._free.push(c);
            this._scheduleIdle(c);
        }
    }

    async query(sql, params, opts) {
        const c = await this.acquire();
        try {
            return await c.query(sql, params, opts);
        } catch (e) {
            this._retire(c);
            throw e;
        } finally {
            if (this._used.has(c)) {
                this.release(c);
            }
        }
    }

    async pipeline(stmts) {
        const c = await this.acquire();
        let ok = false;
        try {
            const r = await c.pipeline(stmts);
            ok = true;
            return r;
        } finally {
            if (ok) this.release(c);
            else this._retire(c);
        }
    }

    async with(fn) {
        const c = await this.acquire();
        let ok = false;
        try {
            const r = await fn(c);
            ok = true;
            return r;
        } finally {
            if (ok) this.release(c);
            else this._retire(c);
        }
    }

    async close() {
        if (this._closed) return;
        this._closed = true;
        const err = new RangeError("PgPool: closed");
        while (this._waiters.length) {
            const w = this._waiters.shift();
            try { w.reject(err); } catch (_) {}
        }
        for (const c of this._free) {
            this._cancelIdle(c);
            this._discard(c);
        }
        this._free.length = 0;
    }
}

export class RedisPool {
    constructor(opts = {}) {
        this._opts = {};
        for (const k of ["host","port","path","user","password","protocol","tls"])
            if (opts[k] !== undefined) this._opts[k] = opts[k];
        this.size = Math.max(1, opts.size | 0 || 4);
        this.acquireTimeoutMs = Math.max(1, opts.acquireTimeoutMs | 0 || 5000);
        this._free = [];
        this._used = new Set();
        this._waiters = [];
        this._closed = false;
    }

    get stats() {
        return {
            total: this._free.length + this._used.size,
            free: this._free.length,
            used: this._used.size,
            waiting: this._waiters.length,
            closed: this._closed,
        };
    }

    _spawn() {
        return new Redis(this._opts);
    }

    _discard(c) { try { c.close(); } catch (_) {} }

    _retire(c) {
        this._used.delete(c);
        const fi = this._free.indexOf(c);
        if (fi >= 0) this._free.splice(fi, 1);
        this._discard(c);
        if (this._waiters.length && !this._closed) {
            const w = this._waiters.shift();
            try {
                const nc = this._spawn();
                w.resolve(nc);
            } catch (err) {
                w.reject(err);
            }
        }
    }

    async acquire() {
        if (this._closed) throw new RangeError("RedisPool: closed");
        if (this._free.length) {
            const c = this._free.pop();
            this._used.add(c);
            return c;
        }
        if (this._free.length + this._used.size < this.size) {
            const c = this._spawn();
            this._used.add(c);
            return c;
        }
        return await new Promise((resolve, reject) => {
            const w = {
                resolve: (c) => { clearTimeout(timer); this._used.add(c); resolve(c); },
                reject: (e) => { clearTimeout(timer); reject(e); },
            };
            const timer = setTimeout(() => {
                const i = this._waiters.indexOf(w);
                if (i >= 0) this._waiters.splice(i, 1);
                w.reject(new RangeError(`RedisPool: acquire timed out after ${this.acquireTimeoutMs}ms`));
            }, this.acquireTimeoutMs);
            this._waiters.push(w);
        });
    }

    release(c) {
        if (!this._used.has(c)) {
            if (this._free.includes(c)) return;
            this._discard(c);
            return;
        }
        this._used.delete(c);
        const fi = this._free.indexOf(c);
        if (fi >= 0) this._free.splice(fi, 1);
        if (this._closed) { this._discard(c); return; }
        if (this._waiters.length) {
            const w = this._waiters.shift();
            w.resolve(c);
        } else {
            this._free.push(c);
        }
    }

    async command(cmd, ...args) {
        const c = await this.acquire();
        let ok = false;
        try {
            const r = await c.command(cmd, ...args);
            ok = true;
            return r;
        } finally {
            if (ok) this.release(c);
            else this._retire(c);
        }
    }

    async pipeline(cmds) {
        const c = await this.acquire();
        let ok = false;
        try {
            const r = await c.pipeline(cmds);
            ok = true;
            return r;
        } finally {
            if (ok) this.release(c);
            else this._retire(c);
        }
    }

    async close() {
        if (this._closed) return;
        this._closed = true;
        const err = new RangeError("RedisPool: closed");
        while (this._waiters.length) this._waiters.shift().reject(err);
        while (this._free.length) this._discard(this._free.pop());
    }
}

if (typeof PostgreSQL !== "undefined" && !PostgreSQL.prototype.queryIter) {
    PostgreSQL.prototype.queryIter = async function* (sql, params, opts = {}) {
        const batch = Math.max(1, opts.batch | 0 || 1000);
        const maxRows = opts.maxRows | 0 || 0;
        const name = `_dqi${++_seq}`;
        let n = 0;
        let ok = false;
        await this.query("BEGIN");
        try {
            await this.query(`DECLARE ${name} CURSOR FOR ${sql}`, params);
            while (true) {
                let res;
                try {
                    res = await this.query(`FETCH FORWARD ${batch} FROM ${name}`);
                } catch (e) {
                    e.message = `FETCH ${name} batch ${batch}: ${e.message}`;
                    throw e;
                }
                const rows = res.rows ?? res;
                if (!rows || !rows.length) break;
                for (const row of rows) {
                    if (maxRows && n >= maxRows) break;
                    n++;
                    yield row;
                    if (maxRows && n >= maxRows) break;
                }
                if (maxRows && n >= maxRows) break;
            }
            ok = true;
        } finally {
            try { await this.query(`CLOSE ${name}`); } catch (_) {}
            try { await this.query(ok ? "COMMIT" : "ROLLBACK"); } catch (_) {}
        }
    };
}
