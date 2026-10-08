// 20 · Layered configuration — defaults, a TOML file, a .env file and the environment, validated by a schema.
//
// WHAT IT SHOWS
//   - dyna:config TOML and Env parsers
//   - precedence done explicitly: defaults < file < .env < real environment
//   - dyna:schema: the merged result is validated once, so the rest of the program can trust it
//   - dyna:json Pointer: mapping flat environment names onto nested settings
//   - secrets redacted whenever the configuration is printed
//
// RUN      dynajs examples/apps/20-layered-config-loader.js [config.toml] [.env]

import { TOML, Env } from "dyna:config";
import { Schema } from "dyna:schema";
import { Pointer } from "dyna:json";
import { Path, makeTempDir, writeFile, readFile, exists, removeAll } from "dyna:file";
import { env as processEnv } from "dyna:sys";
import { parseDurationMs } from "dyna:time";

const DEFAULTS = {
    server: { host: "127.0.0.1", port: 8080, requestTimeout: "30s" },
    database: { url: "sqlite::memory:", poolSize: 4 },
    log: { level: "info" },
};

// Which environment variable overrides which setting, and how to read it.
const ENV_MAP = {
    APP_HOST:        ["/server/host", String],
    APP_PORT:        ["/server/port", Number],
    APP_TIMEOUT:     ["/server/requestTimeout", String],
    DATABASE_URL:    ["/database/url", String],
    DB_POOL_SIZE:    ["/database/poolSize", Number],
    LOG_LEVEL:       ["/log/level", String],
};
const SECRET_POINTERS = ["/database/url"];

const schema = Schema.compile({
    type: "object",
    required: ["server", "database", "log"],
    properties: {
        server: {
            type: "object", additionalProperties: false,
            properties: {
                host: { type: "string", minLength: 1 },
                port: { type: "integer", minimum: 1, maximum: 65535 },
                requestTimeout: { type: "string", pattern: "^[0-9.]+(ms|s|m|h)$" },
            },
        },
        database: {
            type: "object", additionalProperties: false,
            properties: { url: { type: "string", minLength: 1 }, poolSize: { type: "integer", minimum: 1, maximum: 64 } },
        },
        log: {
            type: "object", additionalProperties: false,
            properties: { level: { enum: ["debug", "info", "warn", "error"] } },
        },
    },
});

// Merge plain objects section by section; arrays and scalars replace.
function merge(base, over) {
    const out = structuredClone(base);
    for (const [key, value] of Object.entries(over ?? {}))
        out[key] = value && typeof value === "object" && !Array.isArray(value) ? merge(out[key] ?? {}, value) : value;
    return out;
}

function loadConfig({ tomlPath, envPath, environment }) {
    let config = structuredClone(DEFAULTS);
    if (tomlPath && exists(tomlPath)) config = merge(config, TOML.parse(readFile(tomlPath)));

    // .env supplies values the real environment does not already define. It is
    // parsed, not loaded: this function has no side effects on the process.
    const dotenv = envPath && exists(envPath) ? Env.parse(readFile(envPath)) : {};
    const vars = { ...dotenv, ...environment };
    for (const [name, [pointer, convert]] of Object.entries(ENV_MAP))
        if (vars[name] !== undefined && vars[name] !== "") config = Pointer.set(config, pointer, convert(vars[name]));

    const verdict = schema.validate(config);
    if (!verdict.valid) {
        const first = verdict.errors[0];
        throw new Error(`invalid configuration at ${first.instancePath ?? first.path ?? "?"}: ${first.keyword}`);
    }
    // Derived values are computed once here rather than re-parsed at every use.
    config.server.requestTimeoutMs = parseDurationMs(config.server.requestTimeout);
    return Object.freeze(config);
}

function redacted(config) {
    let copy = structuredClone(config);
    for (const pointer of SECRET_POINTERS) copy = Pointer.set(copy, pointer, "<redacted>");
    return copy;
}

// ---- command line / self-test ---------------------------------------------
const args = scriptArgs.slice(1);
if (args.length) {
    const config = loadConfig({ tomlPath: new Path(args[0]), envPath: args[1] && new Path(args[1]), environment: processEnv() });
    console.log(JSON.stringify(redacted(config), null, 2));
} else {
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
    const dir = makeTempDir("config");
    const tomlPath = dir.join("app.toml");
    const envPath = dir.join(".env");
    writeFile(tomlPath, '[server]\nport = 9000\nrequestTimeout = "90s"\n\n[log]\nlevel = "debug"\n');
    writeFile(envPath, "DATABASE_URL=postgres://app:s3cret@db.internal/app\nDB_POOL_SIZE=8\nAPP_PORT=9100\n");

    const fromFiles = loadConfig({ tomlPath, envPath, environment: {} });
    check(fromFiles.server.host === "127.0.0.1", "defaults fill what nothing overrides");
    check(fromFiles.log.level === "debug", "the TOML file overrides defaults");
    check(fromFiles.server.port === 9100, ".env overrides the file");
    check(fromFiles.server.requestTimeoutMs === 90000, "durations are parsed once");

    const withEnv = loadConfig({ tomlPath, envPath, environment: { APP_PORT: "443", LOG_LEVEL: "warn" } });
    check(withEnv.server.port === 443 && withEnv.log.level === "warn", "the real environment wins over .env");

    for (const bad of [{ APP_PORT: "70000" }, { LOG_LEVEL: "loud" }, { DB_POOL_SIZE: "many" }]) {
        let message = "";
        try { loadConfig({ tomlPath, envPath, environment: bad }); } catch (e) { message = e.message; }
        check(message.startsWith("invalid configuration"), "refused: " + JSON.stringify(bad));
    }
    check(!JSON.stringify(redacted(fromFiles)).includes("s3cret"), "secrets never reach the printed form");
    check(Object.isFrozen(fromFiles), "the loaded configuration is immutable");
    console.log("self-test passed:", JSON.stringify(redacted(withEnv).server));
    removeAll(dir);
}
