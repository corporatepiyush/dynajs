(function(streamsInternal) {
    
    function strictInit(init, valid) {
        if (init === null || typeof init !== 'object') return;
        for (const k of Object.keys(init)) {
            if (valid.indexOf(k) < 0)
                throw new TypeError('unknown option "' + k + '" (valid: ' + valid.join(', ') + ')');
        }
    }
    const REQUEST_INIT_KEYS = ['method', 'headers', 'body', 'signal', 'redirect'];
    const RESPONSE_INIT_KEYS = ['status', 'statusText', 'headers', 'url'];
    const FETCH_INIT_KEYS = ['method', 'headers', 'body', 'signal', 'redirect', 'timeout', 'onConnect', 'cookieJar'];

    class Headers {
        #map = new Map();
        constructor(init) {
            if (init) {
                if (init instanceof Headers) {
                    for (const [k, v] of init) this.append(k, v);
                } else if (Array.isArray(init)) {
                    for (const [k, v] of init) this.append(k, v);
                } else if (typeof init === 'object') {
                    for (const k of Object.keys(init)) this.append(k, init[k]);
                }
            }
        }
        append(name, value) {
            name = String(name).toLowerCase();
            value = String(value).trim();
            if (this.#map.has(name)) this.#map.set(name, this.#map.get(name) + ', ' + value);
            else this.#map.set(name, value);
        }
        delete(name) { this.#map.delete(String(name).toLowerCase()); }
        get(name) { return this.#map.get(String(name).toLowerCase()) ?? null; }
        has(name) { return this.#map.has(String(name).toLowerCase()); }
        set(name, value) { this.#map.set(String(name).toLowerCase(), String(value).trim()); }
        forEach(cb, thisArg) { for (const [k, v] of this.#map) cb.call(thisArg, v, k, this); }
        *keys() { yield* this.#map.keys(); }
        *values() { yield* this.#map.values(); }
        *entries() { yield* this.#map.entries(); }
        [Symbol.iterator]() { return this.entries(); }
    }

    class FormData {
        #entries = [];
        append(name, value, filename) { this.#entries.push([String(name), value, filename]); }
        delete(name) { name = String(name); this.#entries = this.#entries.filter(e => e[0] !== name); }
        get(name) { name = String(name); const e = this.#entries.find(x => x[0] === name); return e ? e[1] : null; }
        getAll(name) { name = String(name); return this.#entries.filter(x => x[0] === name).map(x => x[1]); }
        has(name) { name = String(name); return this.#entries.some(x => x[0] === name); }
        set(name, value, filename) {
            name = String(name);
            let rep = false;
            this.#entries = this.#entries.map(e => {
                if (e[0] === name) { if (!rep) { rep = true; return [name, value, filename]; } return null; }
                return e;
            }).filter(Boolean);
            if (!rep) this.#entries.push([name, value, filename]);
        }
        forEach(cb, thisArg) { for (const [k, v] of this.#entries) cb.call(thisArg, v, k, this); }
        *keys() { for (const [k] of this.#entries) yield k; }
        *values() { for (const [, v] of this.#entries) yield v; }
        *entries() { for (const [k, v] of this.#entries) yield [k, v]; }
        [Symbol.iterator]() { return this.entries(); }
    }

    class AbortSignal {
        #aborted = false;
        #reason = undefined;
        #listeners = [];
        get aborted() { return this.#aborted; }
        get reason() { return this.#reason; }
        get onabort() { return this._onabort; }
        set onabort(fn) { this._onabort = fn; }
        addEventListener(type, listener) {
            if (type === 'abort' && typeof listener === 'function') {
                if (this.#aborted) queueMicrotask(() => listener.call(this, { type: 'abort', target: this }));
                else this.#listeners.push(listener);
            }
        }
        removeEventListener(type, listener) {
            if (type === 'abort') this.#listeners = this.#listeners.filter(l => l !== listener);
        }
        throwIfAborted() { if (this.#aborted) throw this.#reason; }
        _abort(reason) {
            if (this.#aborted) return;
            this.#aborted = true;
            this.#reason = reason !== undefined ? reason : new Error('This operation was aborted');
            const evt = { type: 'abort', target: this };
            if (typeof this._onabort === 'function') {
                try { this._onabort.call(this, evt); } catch (e) { queueMicrotask(() => { throw e; }); }
            }
            for (const l of this.#listeners) {
                try { l.call(this, evt); } catch (e) { queueMicrotask(() => { throw e; }); }
            }
        }
        static abort(reason = undefined) {
            const s = new AbortSignal();
            s._abort(reason);
            return s;
        }
        static timeout(delayMs) {
            const c = new AbortController();
            const t = setTimeout(() => c.abort(new Error('The operation timed out')), delayMs);
            c.signal.addEventListener('abort', () => clearTimeout(t));
            
            c.signal._cancelTimeout = () => clearTimeout(t);
            return c.signal;
        }
    }

    class AbortController {
        #signal = new AbortSignal();
        get signal() { return this.#signal; }
        abort(reason) { this.#signal._abort(reason); }
    }

    const ownedBodies = new WeakSet();
    let SI;
    const si = () => SI ?? (SI = streamsInternal(false));
    const isStream = (b) => b !== null && typeof b === 'object' && streamsInternal(true) !== undefined && si().rsOf(b) !== undefined;
    const STREAM = {};
    function bodyBytes(b, owned) {
        if (b === null || b === undefined) return new Uint8Array(0);
        if (b instanceof Uint8Array) return b.slice();
        if (b instanceof ArrayBuffer) return new Uint8Array(owned ? b : b.slice(0));
        if (typeof b === 'string') return new TextEncoder().encode(b);
        return new TextEncoder().encode(String(b));
    }
    class Response {
        #status; #statusText; #headers; #body; #bodyUsed = false; #url = ''; #stream = null;
        constructor(body = null, init = {}) {
            strictInit(init, RESPONSE_INIT_KEYS);
            this.#status = init.status ?? 200;
            this.#statusText = init.statusText ?? 'OK'; 
            this.#headers = init.headers instanceof Headers ? init.headers : new Headers(init.headers);
            this.#url = init.url ?? '';
            if (isStream(body)) {
                if (si().isLocked(body) || si().isDisturbed(body)) throw new TypeError('Response body stream is locked or disturbed');
                this.#stream = body;
                this.#body = null;
            } else {
                this.#body = body;
            }
        }
        get body() {
            if (this.#stream !== null) return this.#stream;
            const b = this.#body;
            if (b === null || b === undefined) return null;
            this.#stream = si().bodyStream(this.#bodyUsed ? new Uint8Array(0) : bodyBytes(b, ownedBodies.has(this)), this.#bodyUsed);
            this.#body = null;
            return this.#stream;
        }
        get status() { return this.#status; }
        get statusText() { return this.#statusText; }
        get ok() { return this.#status >= 200 && this.#status < 300; }
        get headers() { return this.#headers; }
        get url() { return this.#url; }
        get bodyUsed() { return this.#bodyUsed || (this.#stream !== null && si().isDisturbed(this.#stream)); }
        #consume() {
            if (this.bodyUsed) throw new TypeError('Body has already been consumed');
            if (this.#stream !== null) {
                if (si().isLocked(this.#stream)) throw new TypeError('Body stream is locked');
                this.#bodyUsed = true;
                return STREAM;
            }
            this.#bodyUsed = true;
            return this.#body;
        }
        async text() {
            const b = this.#consume();
            if (b === STREAM) return new TextDecoder().decode(await si().bodyReadAll(this.#stream));
            if (b === null || b === undefined) return '';
            if (typeof b === 'string') return b;
            if (b instanceof Uint8Array || b instanceof ArrayBuffer) return new TextDecoder().decode(b);
            return String(b);
        }
        async json() { return JSON.parse(await this.text()); }
        async bytes() {
            const b = this.#consume();
            if (b === STREAM) return si().bodyReadAll(this.#stream);
            if (b === null || b === undefined) return new Uint8Array(0);
            
            if (b instanceof Uint8Array) return b.slice();
            if (b instanceof ArrayBuffer) return new Uint8Array(ownedBodies.has(this) ? b : b.slice(0));
            if (typeof b === 'string') return new TextEncoder().encode(b);
            return new TextEncoder().encode(String(b));
        }
        async arrayBuffer() {
            const u8 = await this.bytes();
            if (u8.byteOffset === 0 && u8.byteLength === u8.buffer.byteLength) return u8.buffer;
            return u8.buffer.slice(u8.byteOffset, u8.byteOffset + u8.byteLength);
        }
        clone() {
            if (this.bodyUsed) throw new TypeError('Cannot clone a consumed response');
            let body = this.#body;
            if (this.#stream !== null) {
                if (si().isLocked(this.#stream)) throw new TypeError('Cannot clone a response whose body stream is locked');
                const [a, b] = si().tee(this.#stream);
                this.#stream = a;
                body = b;
            }
            
            if (body instanceof Uint8Array) body = body.slice();
            else if (body instanceof ArrayBuffer) body = body.slice(0);
            return new Response(body, {
                status: this.#status, statusText: this.#statusText,
                headers: new Headers(this.#headers), url: this.#url
            });
        }
    }

    class Request {
        #method; #url; #headers; #body; #signal; #redirect;
        constructor(input, init = {}) {
            strictInit(init, REQUEST_INIT_KEYS);
            if (input instanceof Request) {
                this.#url = input.url;
                this.#method = init.method ?? input.method;
                this.#headers = new Headers(init.headers ?? input.headers);
                this.#body = init.body ?? input.#body;
                this.#signal = init.signal ?? input.#signal;
                this.#redirect = init.redirect ?? input.#redirect;
            } else {
                this.#url = String(input);
                this.#method = (init.method ?? 'GET').toUpperCase();
                this.#headers = new Headers(init.headers);
                this.#body = init.body ?? null;
                this.#signal = init.signal ?? null;
                this.#redirect = init.redirect ?? 'follow';
            }
            if (['follow', 'manual', 'error'].indexOf(this.#redirect) < 0) {
                throw new TypeError('Request: redirect must be "follow", "manual" or "error"');
            }
        }
        get method() { return this.#method; }
        get url() { return this.#url; }
        get headers() { return this.#headers; }
        get signal() { return this.#signal; }
        get redirect() { return this.#redirect; }
        get body() { return this.#body; }
        async text() {
            if (isStream(this.#body)) return new TextDecoder().decode(await si().bodyReadAll(this.#body));
            if (this.#body === null || this.#body === undefined) return '';
            if (typeof this.#body === 'string') return this.#body;
            if (this.#body instanceof Uint8Array || this.#body instanceof ArrayBuffer) return new TextDecoder().decode(this.#body);
            return String(this.#body);
        }
        async json() { return JSON.parse(await this.text()); }
        async bytes() {
            if (isStream(this.#body)) return si().bodyReadAll(this.#body);
            if (this.#body === null || this.#body === undefined) return new Uint8Array(0);
            if (this.#body instanceof Uint8Array) return this.#body.slice();
            if (this.#body instanceof ArrayBuffer) return new Uint8Array(this.#body.slice(0));
            return new TextEncoder().encode(String(this.#body));
        }
        async arrayBuffer() {
            const u8 = await this.bytes();
            return u8.buffer.slice(u8.byteOffset, u8.byteOffset + u8.byteLength);
        }
    }

    
    function rawHeader(h, name) {
        const key = Object.keys(h).find(k => k.toLowerCase() === name);
        return key === undefined ? null : h[key];
    }
    var _URLC = null;
    async function urlCtor() {
        if (!_URLC) {
            try { _URLC = (await import('dyna:url')).URL; }
            catch (e) { _URLC = globalThis.URL || null; }
        }
        return _URLC;
    }

    
    async function redirectHop(client, raw, url, method, body, headersObj, hop) {
        if ([301, 302, 303, 307, 308].indexOf(raw.status) < 0) return null;
        const loc = rawHeader(raw.headers, 'location');
        if (loc === null || loc === '') return null;
        let next;
        try {
            const C = await urlCtor();
            if (C) next = new C(loc, url).href;
            else if (/^https?:\/\//i.test(loc)) next = loc;
            else if (loc[0] === '/') {
                const m = /^https?:\/\/[^/\/?#]*/i.exec(url);
                if (!m) throw new Error('no base');
                next = m[0] + loc;
            } else {
                const base = url.slice(0, url.lastIndexOf('/') + 1);
                next = base + loc;
            }
        } catch (e) {
            throw new TypeError('fetch: invalid Location in redirect: ' + loc);
        }
        const plan = client[Symbol.for('dyna.http.redirect')](raw.status, method, url, next, hop);
        if (plan === null) return null;
        const nextBody = plan.dropBody ? undefined : body;
        const nextHeaders = {};
        for (const k of Object.keys(headersObj)) nextHeaders[k] = headersObj[k];
        if (plan.dropBody) {
            for (const k of ['content-type', 'content-encoding', 'content-language', 'content-location'])
                delete nextHeaders[k];
        }
        if (plan.stripCredentials) {
            for (const k of ['authorization', 'cookie', 'cookie2', 'proxy-authorization'])
                delete nextHeaders[k];
        }
        return { url: next, method: plan.method, body: nextBody, headers: nextHeaders };
    }

    async function followHop(client, mode, raw, url, method, body, headersObj, hop) {
        if (mode === 'manual') return null;
        const hop2 = await redirectHop(client, raw, url, method, body, headersObj, hop);
        if (hop2 !== null && mode === 'error') {
            throw new TypeError('fetch: redirect to ' + hop2.url + ' refused by redirect: "error"');
        }
        return hop2;
    }

    function responseFrom(raw, url) {
        
        const r = new Response(raw.bodyBytes ?? raw.body, {
            status: raw.status, statusText: raw.statusText,
            headers: new Headers(raw.headers), url
        });
        if (raw.bodyBytes instanceof ArrayBuffer) ownedBodies.add(r);
        return r;
    }

    
    async function fetchImpl(input, init = {}, timeoutMs, onConnect, cookieJar) {
        const req = new Request(input, init);
        const signal = req.signal;
        if (signal && signal.aborted) throw signal.reason;

        let url = req.url;
        let method = req.method;

        let headersObj = {};
        for (const [k, v] of req.headers) headersObj[k] = v;

        let body = undefined;
        if (method !== 'GET' && method !== 'HEAD') {
            const b = req.body;
            if (b instanceof FormData) {
                const { MultipartFormat } = await import('dyna:http');
                const parts = [];
                for (const [name, val, filename] of b.entries()) {
                    if (val instanceof Uint8Array || val instanceof ArrayBuffer) {
                        parts.push({ name, body: val, filename: filename || undefined });
                    } else {
                        parts.push({ name, value: String(val), filename: filename || undefined });
                    }
                }
                const fmt = MultipartFormat(parts);
                body = fmt.body;
                headersObj['content-type'] = fmt.contentType;
            } else if (isStream(b)) {
                body = await si().bodyReadAll(b);
            } else if (b !== null && b !== undefined) {
                body = typeof b === 'string' ? b :
                    (b instanceof Uint8Array || b instanceof ArrayBuffer
                     || b instanceof DataView ? b : await req.text());
            }
        }

        const { HTTPClient } = await import('dyna:http');
        const client = new HTTPClient();
        if (timeoutMs) client.setTimeout(timeoutMs);
        if (typeof onConnect === 'function') client.onConnect = onConnect;
        if (cookieJar !== undefined && cookieJar !== null) client.useCookies(cookieJar);

        
        for (let redirects = 0; ; redirects++) {

            let abortListener;
            let reqPromise = client[Symbol.for('dyna.http.request')](method, url, body, headersObj, true);

            if (signal) {
                
                reqPromise.catch(() => {});
                const abortPromise = new Promise((_, reject) => {
                    abortListener = () => {
                        try { client.disconnect(); } catch (e) {}
                        reject(signal.reason);
                    };
                    signal.addEventListener('abort', abortListener);
                });
                let raw;
                try {
                    raw = await Promise.race([reqPromise, abortPromise]);
                } finally {
                    if (signal._cancelTimeout) try { signal._cancelTimeout(); } catch (e) {}
                    if (abortListener) signal.removeEventListener('abort', abortListener);
                }
                const hop = await followHop(client, req.redirect, raw, url, method, body, headersObj, redirects);
                if (hop === null) return responseFrom(raw, url);
                url = hop.url; method = hop.method; body = hop.body;
                headersObj = hop.headers;
            } else {
                const raw = await reqPromise;
                const hop = await followHop(client, req.redirect, raw, url, method, body, headersObj, redirects);
                if (hop === null) return responseFrom(raw, url);
                url = hop.url; method = hop.method; body = hop.body;
                headersObj = hop.headers;
            }
        }
    }
    function fetch(input, init) {
        strictInit(init, FETCH_INIT_KEYS);
        let bag = init, tmo, oc, jar;
        if (bag !== null && typeof bag === 'object'
            && (bag.timeout !== undefined || bag.onConnect !== undefined || bag.cookieJar !== undefined)) {
            tmo = bag.timeout;
            oc = bag.onConnect;
            jar = bag.cookieJar;
            bag = Object.assign({}, bag);
            delete bag.timeout;
            delete bag.onConnect;
            delete bag.cookieJar;
        }
        return fetchImpl(input, bag, tmo, oc, jar);
    }
    var _g = typeof globalThis !== 'undefined' ? globalThis : (typeof window !== 'undefined' ? window : this);
    _g.Headers = Headers;
    _g.FormData = FormData;
    _g.AbortSignal = AbortSignal;
    _g.AbortController = AbortController;
    _g.Response = Response;
    _g.Request = Request;
    _g.fetch = fetch;
})
