import { test, run, assert, assertEqual, deepEqual } from "./harness.js";

function escapeLiteral(s) {
  return s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

export function compileRoute(pattern) {
  const paramNames = [];
  let src = "";
  let i = 0;
  while (i < pattern.length) {
    const c = pattern[i];
    if (c === ":") {
      const m = /^:([A-Za-z_]\w*)(\(([^)]*)\))?/.exec(pattern.slice(i));
      if (!m) throw new SyntaxError(`bad parameter at index ${i}`);
      const [whole, name, , custom] = m;
      paramNames.push(name);
      src += `(?<${name}>${custom ?? "[^/]+"})`;
      i += whole.length;
    } else if (c === "*") {
      const m = /^\*([A-Za-z_]\w*)/.exec(pattern.slice(i));
      if (!m) throw new SyntaxError(`bad wildcard at index ${i}`);
      paramNames.push(m[1]);
      src += `(?<${m[1]}>.*)`;
      i += m[0].length;
    } else {
      src += escapeLiteral(c);
      i++;
    }
  }
  return { regex: new RegExp(`^${src}/?$`, "d"), paramNames };
}

export class Router {
  #routes = new Map();

  add(method, pattern, handler) {
    const { regex, paramNames } = compileRoute(pattern);
    const list = this.#routes.get(method) ?? this.#routes.set(method, []).get(method);
    list.push({ pattern, regex, paramNames, handler });
    return this;
  }

  get(pattern, handler) { return this.add("GET", pattern, handler); }
  post(pattern, handler) { return this.add("POST", pattern, handler); }

  match(method, url) {
    const [path, queryString = ""] = url.split("?");
    const routes = this.#routes.get(method);
    if (!routes) return null;

    for (const route of routes) {
      const m = route.regex.exec(path);
      if (!m) continue;

      const params = {};
      const indices = {};
      for (const name of route.paramNames) {
        params[name] = decodeURIComponent(m.groups[name]);
        indices[name] = m.indices.groups[name];
      }
      return { handler: route.handler, params, indices, query: parseQuery(queryString) };
    }
    return null;
  }
}

function parseQuery(qs) {
  const out = {};
  if (!qs) return out;
  for (const pair of qs.split("&")) {
    if (!pair) continue;
    const idx = pair.indexOf("=");
    const k = decodeURIComponent(idx < 0 ? pair : pair.slice(0, idx));
    const v = idx < 0 ? "" : decodeURIComponent(pair.slice(idx + 1));
    out[k] = v;
  }
  return out;
}

function buildRouter() {
  return new Router()
    .get("/", () => "home")
    .get("/users/:id", (p) => `user:${p.id}`)
    .get("/users/:id/posts/:postId", (p) => `post:${p.id}/${p.postId}`)
    .get("/blog/:year(\\d{4})/:slug", (p) => `blog:${p.year}:${p.slug}`)
    .get("/files/:path(.*)", (p) => `file:${p.path}`)
    .get("/assets/*rest", (p) => `asset:${p.rest}`)
    .post("/users", () => "create");
}

test("static and simple param routes", () => {
  const r = buildRouter();
  assertEqual(r.match("GET", "/").handler(), "home");
  const u = r.match("GET", "/users/42");
  assertEqual(u.params.id, "42");
  assertEqual(u.handler(u.params), "user:42");
});

test("multiple params", () => {
  const r = buildRouter();
  const m = r.match("GET", "/users/7/posts/99");
  assertEqual(m.params, { id: "7", postId: "99" });
  assertEqual(m.handler(m.params), "post:7/99");
});

test("d-flag indices point at the right slice of the path", () => {
  const r = buildRouter();
  const path = "/users/ada";
  const m = r.match("GET", path);
  const [start, end] = m.indices.id;
  assertEqual(path.slice(start, end), "ada");
  assertEqual(start, 7);
});

test("typed numeric segment only matches digits", () => {
  const r = buildRouter();
  assertEqual(r.match("GET", "/blog/2024/hello").params, { year: "2024", slug: "hello" });
  assertEqual(r.match("GET", "/blog/abc/hello"), null);
});

test("catch-all and regex-tail params", () => {
  const r = buildRouter();
  assertEqual(r.match("GET", "/assets/img/logo.png").params.rest, "img/logo.png");
  assertEqual(r.match("GET", "/files/a/b/c.txt").params.path, "a/b/c.txt");
});

test("method sensitivity and misses", () => {
  const r = buildRouter();
  assertEqual(r.match("POST", "/users").handler(), "create");
  assertEqual(r.match("GET", "/nope"), null);
  assertEqual(r.match("DELETE", "/users/1"), null);
});

test("query string parsing and percent-decoding", () => {
  const r = buildRouter();
  const m = r.match("GET", "/users/hello%20world?tab=posts&sort=desc&flag");
  assertEqual(m.params.id, "hello world");
  assertEqual(m.query, { tab: "posts", sort: "desc", flag: "" });
});

test("trailing slash is optional", () => {
  const r = buildRouter();
  assertEqual(r.match("GET", "/users/1/").params.id, "1");
  assertEqual(r.match("GET", "/").handler(), "home");
});

test("route priority is insertion order", () => {
  const r = new Router()
    .get("/thing/new", () => "static")
    .get("/thing/:id", (p) => `dynamic:${p.id}`);
  assertEqual(r.match("GET", "/thing/new").handler(), "static");
  assertEqual(r.match("GET", "/thing/42").handler({ id: "42" }), "dynamic:42");
});

await run("regex router");
