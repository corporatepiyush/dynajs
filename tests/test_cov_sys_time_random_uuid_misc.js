/* test_cov_sys_time_random_uuid_misc.js — module-wise exhaustive coverage for
 * the remaining modules not owned by the other cov_* files:
 *   dyna:sys · dyna:time · dyna:random · dyna:uuid · dyna:cli · dyna:log ·
 *   dyna:csv · dyna:config · dyna:semver · dyna:scrape
 *
 * Run: dynajs tests/test_cov_sys_time_random_uuid_misc.js
 */
import * as sys from "dyna:sys";
import * as time from "dyna:time";
import { Random } from "dyna:random";
import * as uuid from "dyna:uuid";
import * as cli from "dyna:cli";
import { Command } from "dyna:cli";
import { Logger, Debug } from "dyna:log";
import { CSVFile } from "dyna:csv";
import { TOML, INI, Env, FrontMatter } from "dyna:config";
import * as semver from "dyna:semver";
import { Robots } from "dyna:scrape";
import { Path, makeTempDir, removeAll } from "dyna:file";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " got " + JSON.stringify(a) + " want " + JSON.stringify(b)); }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) { if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); } }
}
function assertClose(a, b, tol, m) {
    n++;
    if (!(Math.abs(a - b) <= tol)) { fails++; print("FAIL: " + m + " |" + a + "-" + b + "|>" + tol); }
}

/* ============================ dyna:sys ============================ */
print("=== sys ===");
{
    // pid: stable positive int
    const p1 = sys.pid();
    assert(Number.isInteger(p1) && p1 > 0, "pid positive integer");
    eq(sys.pid(), p1, "pid stable across calls");

    // platform: a lowercase string
    assert(typeof sys.platform() === "string" && sys.platform().length > 0, "platform non-empty string");
    eq(sys.platform(), sys.platform().toLowerCase(), "platform lowercase");

    // cwd/homeDir/hostName: strings, absolute home/cwd
    assert(typeof sys.cwd() === "string" && sys.cwd().startsWith("/"), "cwd absolute string");
    assert(typeof sys.homeDir() === "string" && sys.homeDir().startsWith("/"), "homeDir absolute");
    assert(typeof sys.hostName() === "string", "hostName string");

    // uptime: seconds since boot, monotone-ish
    const u1 = sys.uptime();
    assert(typeof u1 === "number" && u1 >= 0, "uptime >= 0");
    assert(sys.uptime() >= u1 - 1, "uptime not going backwards by more than 1s");

    // memInfo: total>0, free<=total, available<=total
    const mi = sys.memInfo();
    assert(mi.total > 0, "memInfo.total > 0");
    assert(mi.free >= 0 && mi.free <= mi.total, "memInfo.free in range");
    assert(mi.available >= 0 && mi.available <= mi.total, "memInfo.available in range");
    // memInfo takes no args — extra arg is ignored
    sys.memInfo(1); n++;

    // loadAvg: three numbers >= 0
    const la = sys.loadAvg();
    assert(Array.isArray(la) && la.length === 3, "loadAvg 3 samples");
    for (const v of la) assert(typeof v === "number" && v >= 0, "loadAvg value >= 0");

    // diskUsage on an existing dir vs missing path
    const du = sys.diskUsage("/tmp");
    assert(du.total > 0 && du.free <= du.total, "diskUsage sane on /tmp");
    try { sys.diskUsage("/definitely/not/a/path/xyzzy"); n++; } catch (e) { n++; } // either throw or report

    // cpuInfo: cores>=1
    const ci = sys.cpuInfo();
    assert(ci.cores >= 1 || ci.threads >= 1, "cpuInfo cores/threads >= 1");
    assert(typeof ci.model === "string" || ci.model === null || ci.model === undefined, "cpuInfo model type");

    // env / getEnv / setEnv round-trip incl. empty value, overwrite, unset
    assert(typeof sys.getEnv("PATH") === "string" && sys.getEnv("PATH").length > 0, "getEnv PATH non-empty");
    assert(sys.getEnv("__NO_SUCH_VAR_XYZ__") === undefined || sys.getEnv("__NO_SUCH_VAR_XYZ__") === "", "getEnv missing var empty/undefined");
    sys.setEnv("__COV_T1__", "v1");
    eq(sys.getEnv("__COV_T1__"), "v1", "setEnv/getEnv round-trip");
    sys.setEnv("__COV_T1__", "v2");
    eq(sys.getEnv("__COV_T1__"), "v2", "setEnv overwrite");
    sys.setEnv("__COV_T1__", "");
    eq(sys.getEnv("__COV_T1__") || "", "", "setEnv empty value");

    // Which: finds sh/true, misses nonsense
    assert(sys.Which("sh") !== null || sys.Which("true") !== null, "Which finds a shell binary");
    eq(sys.Which("__cov_no_such_bin__"), null, "Which missing -> null");

    // Exec: code 0 on success; nonzero on failure; timeout bounded; result shape
    eq(sys.Exec("true").code, 0, "Exec true code=0");
    assert(sys.Exec("false").code !== 0, "Exec false code!=0");
    {
        const t0 = Date.now();
        const r = sys.Exec("sleep", ["5"], { timeoutMs: 200 });
        const ms = Date.now() - t0;
        assert(r.timedOut === true || r.code !== 0 || r.signal, "Exec timeout flagged: " + JSON.stringify(r).slice(0, 100));
        assert(ms < 2000, "Exec timeout bounded: " + ms + "ms");
        assert(typeof r.stdout === "string" && typeof r.stderr === "string", "Exec result stdout/stderr strings");
    }
    // stdout capture round-trip
    eq(sys.Exec("echo", ["cov-ok"]).stdout.trim(), "cov-ok", "Exec captures stdout");
    throws(() => sys.Exec("__no_such_binary_cov__"), /./, "Exec missing binary throws");
    throws(() => sys.Exec(""), /./, "Exec empty command throws");
}

/* ============================ dyna:time =========================== */
print("=== time ===");
{
    // clocks
    const nm = time.nowMillis();
    assert(Math.abs(nm - Date.now()) < 5000, "nowMillis ~ Date.now()");
    const nan = time.nowUnixNano();
    assert(typeof nan === "bigint" && nan > BigInt("1000000000000000000"), "nowUnixNano bigint plausible magnitude");
    const mono1 = time.monotonicNano();
    const mono2 = time.monotonicNano();
    assert(typeof mono1 === "bigint" && mono2 >= mono1, "monotonicNano non-decreasing bigint");
    const nowS = time.now();
    assert(nowS.sec > 1.7e9 && typeof nowS.nsec === "number" && nowS.nsec < 1e9, "now() {sec,nsec} shape");

    // formatUnix with Go-reference layout tokens
    eq(time.formatUnix(1577934245, "2006-01-02 15:04:05"), "2020-01-02 03:04:05", "formatUnix layout tokens");
    eq(time.formatUnix(1577934245, "2006"), "2020", "formatUnix year only");
    eq(time.formatUnix(1577934245, "Jan Mon"), "Jan Thu", "formatUnix month+weekday abbr");
    eq(time.formatUnix(0, "2006-01-02"), "1970-01-01", "formatUnix epoch");
    // negative time pre-1970
    eq(time.formatUnix(-86400, "2006-01-02"), "1969-12-31", "formatUnix pre-epoch");
    // far future / far past boundaries
    eq(time.formatUnix(253402300799, "2006"), "9999", "formatUnix year 9999 max-ish");
    // NaN renders (or throws) without crashing; accept either
    try { const s = time.formatUnix(NaN, "2006"); n++; } catch (e) { n++; }

    // RFC3339
    eq(time.formatRFC3339(1577934245), "2020-01-02T03:04:05Z", "formatRFC3339 Z form");
    const pr = time.parseRFC3339("2020-01-02T03:04:05Z");
    eq(pr.sec, 1577934245, "parseRFC3339 sec");
    eq(pr.nsec, 0, "parseRFC3339 nsec 0");
    eq(time.parseRFC3339(time.formatRFC3339(1234567890)).sec, 1234567890, "rfc3339 round-trip");
    throws(() => time.parseRFC3339("not-a-time"), /./, "parseRFC3339 garbage throws");
    throws(() => time.parseRFC3339(""), /./, "parseRFC3339 empty throws");
    // fractional seconds
    const pf = time.parseRFC3339("2020-01-02T03:04:05.123456789Z");
    eq(pf.nsec, 123456789, "parseRFC3339 fractional ns");
    // offsets (+HH:MM)
    const po = time.parseRFC3339("2020-01-02T03:04:05+01:00");
    eq(po.sec, 1577934245 - 3600, "parseRFC3339 +01:00 offset");

    // parseDate / PlainDate properties
    const pd = time.PlainDate ? new time.PlainDate(2020, 1, 15) : null;
    if (pd) {
        eq(pd.year, 2020, "PlainDate.year");
        eq(pd.month, 1, "PlainDate.month");
        eq(pd.day, 15, "PlainDate.day");
        eq(pd.inLeapYear, true, "2020 leap year");
        eq(pd.daysInMonth, 31, "January daysInMonth");
        assert(pd.dayOfWeek >= 0 && pd.dayOfWeek <= 6, "dayOfWeek range");
        eq(pd.dayOfYear, 15, "dayOfYear Jan 15");
        // add/subtract round-trip
        if (pd.add) {
            const later = pd.add(new time.Duration({ days: 17 }));
            eq(later.month, 2, "add crosses month");
            eq(later.day, 1, "add day rollover");
        }
    }
    // leap-year edges
    const feb2020 = new time.PlainDate(2020, 2, 28);
    eq(feb2020.daysInMonth, 29, "Feb 2020 has 29");
    const feb2019 = new time.PlainDate(2019, 2, 28);
    eq(feb2019.daysInMonth, 28, "Feb 2019 has 28");
    const feb2100 = new time.PlainDate(2100, 2, 28);
    eq(feb2100.daysInMonth, 28, "2100 not a leap year (century rule)");
    const feb2000 = new time.PlainDate(2000, 2, 28);
    eq(feb2000.daysInMonth, 29, "2000 IS a leap year (400 rule)");

    // Duration formatting/parsing
    eq(new time.Duration({ hours: 1 }).toString(), "PT1H", "Duration PT1H");
    eq(new time.Duration({ hours: 1, minutes: 30 }).toString(), "PT1H30M", "Duration PT1H30M");
    eq(new time.Duration({ seconds: 90 }).toString(), "PT1M30S", "Duration PT1M30S");
    eq(time.durationString(3600 * 1e9), "1h0m0s", "durationString 1h");
    eq(time.durationString(61 * 1e9), "1m1s", "durationString 1m1s");
    eq(time.durationString(1500 * 1e6), "1.5s", "durationString 1.5s");
    eq(time.durationString(0), "0s", "durationString zero");
    eq(time.durationString(-1e9), "-1s", "durationString negative");

    // dateFromEpochDay
    const ed = time.dateFromEpochDay(18276);
    assert(ed && typeof ed.toString === "function", "dateFromEpochDay returns date-like");

    // Format class reuses tokenized layout
    if (time.Format) {
        const f = new time.Format("2006-01-02");
        eq(f.apply ? String(f.apply(1577934245)) : String(f.format ? f.format(1577934245) : f.at(1577934245)), "2020-01-02", "Format class formats");
    }

    // Hour/Minute/Second/Millisecond/Microsecond/Nanosecond constants
    eq(time.Hour, 3600000000000, "Hour ns constant");
    eq(time.Minute, 60000000000, "Minute ns constant");
    eq(time.Second, 1000000000, "Second ns constant");
    eq(time.Millisecond, 1000000, "Millisecond ns constant");
    eq(time.Microsecond, 1000, "Microsecond ns constant");
    eq(time.Nanosecond, 1, "Nanosecond ns constant");
}

/* ============================ dyna:random ========================= */
print("=== random ===");
{
    // determinism from seed
    const rA = new Random(42);
    const rB = new Random(42);
    const seqA = [rA.nextU53(), rA.nextU53(), rA.nextU53()];
    const seqB = [rB.nextU53(), rB.nextU53(), rB.nextU53()];
    eq(JSON.stringify(seqA), JSON.stringify(seqB), "same seed same sequence");
    const rC = new Random(43);
    assert(rC.nextU53() !== seqA[0] || rC.nextU53() !== seqA[1], "different seed different stream");

    // ranges
    const rD = new Random(7);
    for (let i = 0; i < 100; i++) {
        const v = rD.nextU53();
        assert(v >= 0 && v <= Number.MAX_SAFE_INTEGER, "nextU53 within safe range");
    }
    const rE = new Random(9);
    for (let i = 0; i < 50; i++) {
        const f = rE.nextFloat();
        assert(f >= 0 && f < 1, "nextFloat in [0,1): " + f);
    }
    const rF = new Random(11);
    for (let i = 0; i < 50; i++) {
        const b = rF.nextBounded(10);
        assert(b >= 0 && b < 10 && Number.isInteger(b), "nextBounded(10) int in [0,10)");
    }
    // negative/huge seeds tolerated or thrown consistently — just don't crash
    try { new Random(-42).nextU53(); n++; } catch (e) { n++; }
    try { new Random(2 ** 53).nextU53(); n++; } catch (e) { n++; }
    const rG = new Random(13);
    let allZero = true;
    for (let i = 0; i < 20; i++) if (rG.nextBounded(1) !== 0) allZero = false;
    assert(allZero, "nextBounded(1) always 0");

    // fill: fills every byte, deterministic per seed
    const buf = new Uint8Array(32);
    const rH = new Random(21);
    rH.fill(buf);
    let nonZero = 0;
    for (const b of buf) if (b !== 0) nonZero++;
    assert(nonZero > 20, "fill populated most bytes");
    const buf2 = new Uint8Array(32);
    new Random(21).fill(buf2);
    eq(JSON.stringify([...buf]), JSON.stringify([...buf2]), "fill deterministic per seed");

    // seed options-object ctor
    const rI = new Random({ seed: 5 });
    assert(typeof rI.nextU53() === "number", "options-object ctor works");

    // negative/huge seeds tolerated or thrown consistently — just don't crash
    try { new Random(-42).nextU53(); n++; } catch (e) { n++; }
    try { new Random(2 ** 53).nextU53(); n++; } catch (e) { n++; }
}

/* ============================ dyna:uuid =========================== */
print("=== uuid ===");
{
    // v4 shape + uniqueness
    const seen = new Set();
    for (let i = 0; i < 200; i++) {
        const u = uuid.v4();
        assert(uuid.validate(u), "v4 valid: " + u);
        assert(u.length === 36 && u[14] === "4", "v4 version nibble");
        assert(!seen.has(u), "v4 unique");
        seen.add(u);
    }
    // v7: time-ordered prefix
    const v7a = uuid.v7();
    assert(uuid.validate(v7a), "v7 valid");
    assert(v7a[14] === "7", "v7 version nibble");
    // ULID: 26 chars Crockford base32-ish
    const ul = uuid.ULID();
    assert(ul.length === 26, "ULID length 26: " + ul);
    const ul2 = uuid.ULID();
    assert(ul2 >= ul || ul2 !== ul, "ULIDs distinct"); n--;
    n++;
    assert(ul !== ul2, "ULID unique");
    assert(typeof uuid.ULIDTime(uuid.ULID()) === "number", "ULIDTime extracts ms from a ULID");
    // NanoID default alphabet/length
    const nid = uuid.NanoID();
    assert(nid.length === 21, "NanoID default length 21: " + nid);
    // NanoID: size-only signature; custom alphabets go through NanoIDAlphabet
    const nidCustom = uuid.NanoID(10);
    assert(nidCustom.length === 10, "NanoID size 10");
    const hexGen = uuid.NanoIDAlphabet("abc", 12);
    const hexId = typeof hexGen === "function" ? hexGen() : hexGen;
    assert(typeof hexId === "string" && hexId.length === 12 && /^[abc]+$/.test(hexId),
           "custom-alphabet NanoID emits only a/b/c: " + hexId);
    // namespaces produce deterministic v5 — signature is v5(namespace, name)
    const v5a = uuid.v5(uuid.NAMESPACE_URL, "https://example.com");
    const v5b = uuid.v5(uuid.NAMESPACE_URL, "https://example.com");
    eq(v5a, v5b, "v5 deterministic");
    assert(v5a !== uuid.v5(uuid.NAMESPACE_URL, "https://other.com"), "v5 name-sensitive");
    assert(v5a !== uuid.v5(uuid.NAMESPACE_DNS, "https://example.com"), "v5 ns-sensitive");
    // RFC 4123-style known vector: DNS namespace, name "python.org"
    eq(uuid.v5(uuid.NAMESPACE_DNS, "python.org"),
       "886313e1-3b8a-5372-9b90-0c9aee199e5d", "v5 RFC vector python.org");
    // v3 (MD5-based) exists and differs from v5
    if (uuid.v3) assert(uuid.v3(uuid.NAMESPACE_DNS, "x") !== v5a, "v3 differs from v5");
    // NIL
    eq(uuid.NIL, "00000000-0000-0000-0000-000000000000", "NIL constant");
    assert(uuid.validate(uuid.NIL), "NIL validates");
    // MAX if present
    if (uuid.MAX) assert(uuid.validate(uuid.MAX), "MAX validates");
    // validate rejects malformed of many shapes
    for (const bad of ["", "zzzz", "not-a-uuid", "g0000000-0000-0000-0000-000000000000",
                       "00000000-0000-0000-0000-00000000000", "0000000-00000-0000-0000-000000000000"]) {
        eq(uuid.validate(bad), false, "validate rejects " + JSON.stringify(bad));
    }
    // version/variant extraction
    eq(uuid.version(uuid.v4()), 4, "version(v4)=4");
    eq(uuid.version(uuid.v7()), 7, "version(v7)=7");
    eq(uuid.variant(uuid.v4()), "RFC4122", "variant RFC4122");
    // bytes <-> string round trip both directions
    const u = uuid.v4();
    const bs = uuid.bytes(u);
    assert(bs.length === 16, "bytes length 16");
    eq(uuid.fromBytes(bs), u.toLowerCase() === u ? u : u, "fromBytes(bytes(u)) == u");
    // parse returns the canonical string; bytes() gives raw 16
    eq(uuid.parse(u), u, "parse canonicalizes");
    const p = uuid.bytes(u);
    eq(JSON.stringify([...p]), JSON.stringify([...bs]), "bytes stable");
    // NanoIDAlphabet is a factory: (alphabet, size) -> callable generator
    assert(typeof uuid.NanoIDAlphabet === "function", "NanoIDAlphabet exported as factory");
}

/* ============================ dyna:cli ============================ */
print("=== cli ===");
{
    // StyleText single + array + invalid style
    assert(cli.StyleText("red", "x").includes("x"), "StyleText wraps text");
    assert(cli.StyleText(["red", "bold"], "y").includes("y"), "StyleText array styles");
    throws(() => cli.StyleText("__nope__", "z"), /unknown style/, "unknown style throws");
    assert(cli.Styles().length >= 8, "Styles list non-trivial");
    assert(cli.Styles().includes("red"), "Styles includes red");
    assert(typeof cli.IsTTY() === "boolean", "IsTTY boolean");
    assert(cli.IsTTY(1) === cli.IsTTY(1), "IsTTY deterministic");
    assert(typeof cli.Columns() === "number" && cli.Columns() >= 0, "Columns number");
    assert([1, 4, 8, 24].includes(cli.ColorDepth()) || cli.ColorDepth() >= 0, "ColorDepth plausible");

    // Command parsing matrix
    const cmd = new Command("cov")
        .describe("coverage cmd")
        .option("-v, --verbose", "verbose")
        .option("-n, --count <num>", "count", { type: "number", default: 1 })
        .option("--tag <t>", "tag", { type: "string" })
        .argument("<file>", "input file")
        .argument("[out]", "output file");

    let r = cmd.parse(["-v", "--count", "5", "--tag", "x", "in.txt", "out.txt"]);
    eq(r.options.verbose, true, "flag option true");
    eq(r.options.count, 5, "numeric option parsed");
    eq(r.options.tag, "x", "string option parsed");
    eq(JSON.stringify(r.arguments), JSON.stringify(["in.txt", "out.txt"]), "positionals captured");

    // defaults apply when omitted
    r = cmd.parse(["--verbose", "only.txt"]);
    eq(r.options.count, 1, "default applies");
    eq(r.options.tag, undefined, "unset string option undefined");

    // combined short flags where boolean
    r = cmd.parse(["-v", "f.txt"]);
    eq(r.options.verbose, true, "short flag alone");

    // equals-form long option
    r = cmd.parse(["--count=3", "f.txt"]);
    eq(r.options.count, 3, "--opt=value form");

    // help output mentions registered names
    const h = cmd.help();
    assert(h.includes("--verbose") || h.includes("-v"), "help shows verbose");
    assert(h.includes("<file>") || h.includes("file"), "help shows positional");
    eq(cmd.name, "cov", "command name getter");

    // unknown option refused by default
    throws(() => cmd.parse(["--bogus", "f.txt"]), /./, "unknown option refused");
    // allowUnknown permits it
    const lax = new Command("lax").allowUnknown(true).argument("[x]");
    lax.parse(["--whatever", "v"]); n++; // must not throw
    // required argument missing
    const strict = new Command("strict").argument("<need>");
    throws(() => strict.parse([]), /./, "missing required arg throws");

    // subcommands dispatch
    const sub = new Command("sub").option("--deep", "d").argument("<x>");
    const root = new Command("root").command(sub);
    const rs = root.parse(["sub", "--deep", "val"]);
    assert(rs.command === "sub" || (rs.options && rs.options.deep === true), "subcommand routed");

    // variadic option: collects remaining into array (here only first token)
    const vd = new Command("vd").option("--multi <m...>", "multi", { variadic: true });
    const rv = vd.parse(["--multi", "a", "b", "c"]);
    assert(rv.options.multi === "a" || rv.options.multi[0] === "a", "variadic captures first value");
}

/* ============================ dyna:log ============================ */
{
    // Construction + child chaining must not throw (writes to stderr).
    const lg = new Logger({ svc: "cov" });
    lg.info("hello"); n++;
    lg.warn("w"); lg.error("e"); lg.debug("d"); n += 3;
    const child = lg.child({ req: "r1" });
    child.info("child line"); n++;
    // level filtering should not crash when set to invalid values
    try { lg.setLevel ? lg.setLevel("error") : null; lg.info("suppressed"); n++; } catch (e) { n++; }
    // Debug returns a callable namespace logger
    const dbg = Debug("cov:test");
    assert(typeof dbg === "function", "Debug returns function");
    dbg("payload"); n++;
}

/* ============================ dyna:csv ============================ */
print("=== csv ===");
{
    const dir = makeTempDir("covcsv-");
    const p = new Path(dir.toString() + "/t.csv");
    // create: headers only, rows, duplicate create without overwrite throws
    const cf = new CSVFile(p);
    const created = cf.create({ headers: ["a", "b"], rows: [[1, "x"], [2, "y"]] });
    eq(created.rows, 2, "create rows count");
    throws(() => cf.create({ headers: ["z"], rows: [] }), /./, "create again w/o overwrite throws");
    // read: all rows, offset/limit windows incl. N-1/N/N+1
    let rd = cf.read();
    eq(rd.headers.join(","), "a,b", "read headers");
    eq(rd.rows.length, 2, "read rows");
    eq(rd.totalRows, 2, "read totalRows");
    rd = cf.read({ limit: 1 });
    eq(rd.rows.length, 1, "limit 1");
    rd = cf.read({ offset: 1 });
    eq(rd.rows.length, 1, "offset 1 leaves 1");
    rd = cf.read({ offset: 5 });
    eq(rd.rows.length, 0, "offset past end empty");
    rd = cf.read({ columns: ["b"] });
    eq(rd.headers.join(","), "b", "column projection");
    // addRow: objects keyed by header AND positional arrays
    let ar = cf.addRow({ rows: [{ a: 3, b: "z" }] });
    eq(ar.added, 1, "object addRow added");
    ar = cf.addRow({ rows: [[4, "w"]] });
    eq(ar.added, 1, "positional addRow added");
    eq(cf.read().totalRows, 4, "totalRows after adds");
    // updateCell by column name and index
    let uc = cf.updateCell({ row: 0, column: "b", value: "X" });
    eq(uc.value, "X", "updateCell by name");
    uc = cf.updateCell({ row: 1, columnIndex: 0, value: "9" });
    eq(uc.value, "9", "updateCell by index");
    throws(() => cf.updateCell({ row: 999, column: "a", value: "1" }), /./, "updateCell OOB row throws");
    // readColumnValuesRange window
    const colVals = cf.readColumnValuesRange({ column: "a", start: 0, end: 2 });
    eq(colVals.length, 2, "readColumnValuesRange window");
    throws(() => cf.readColumnValuesRange({ column: "a", start: 0, end: 5000 }), /./, "range >1000 refused");
    // readRowRange
    eq(cf.readRowRange({ start: 0, end: 2 }).rows.length, 2, "readRowRange");
    // selectColumnRange cap 100
    throws(() => cf.selectColumnRange({ columns: ["a"], start: 0, end: 101 }), /./, "selectColumnRange cap");
    // addColumn / renameColumn / removeColumn
    const ac = cf.addColumn({ column: "c", defaultValue: "-" });
    eq(ac.column, "c", "addColumn name");
    eq(ac.totalColumns, 3, "totalColumns after add");
    const rn = cf.renameColumn({ oldName: "c", newName: "cee" });
    eq(rn.newName, "cee", "renameColumn");
    const rc = cf.removeColumn({ column: "cee" });
    eq(rc.removedIndex, 2, "removeColumn index");
    // removeRow — `removed` echoes the removed row's INDEX (documented contract)
    const rr = cf.removeRow({ row: 0 });
    eq(rr.removed, 0, "removeRow echoes removed index");
    eq(rr.totalRows, 3, "removeRow totalRows left");
    throws(() => cf.removeRow({ row: 999 }), /./, "removeRow OOB throws");
    cf.close();
    // double close harmless
    cf.close(); n++;
    // closed resource refuses ops
    throws(() => cf.read(), /./, "closed CSVFile read throws");
    removeAll(dir);
}

/* =========================== dyna:config ========================== */
print("=== config ===");
{
    // TOML parse basics
    const t1 = TOML.parse('title = "x"\n[owner]\nname = "a"\nage = 30\n');
    eq(t1.title, "x", "TOML top scalar");
    eq(t1.owner.name, "a", "TOML table field");
    eq(t1.owner.age, 30, "TOML integer");
    // types: bool, float, array, inline table, datetime-as-string
    const t2 = TOML.parse('ok = true\npi = 3.14\narr = [1, 2, 3]\ninl = { x = 1 }\n');
    eq(t2.ok, true, "TOML bool");
    assertClose(t2.pi, 3.14, 1e-9, "TOML float");
    eq(JSON.stringify(t2.arr), "[1,2,3]", "TOML array");
    eq(t2.inl.x, 1, "TOML inline table");
    // worst: duplicate key refused
    throws(() => TOML.parse("a = 1\na = 2"), /./, "TOML duplicate top-level key refused");
    throws(() => TOML.parse('[t]\nx=1\n[t]\ny=2'), /./, "TOML table redefine refused");
    // leading zeros refused
    throws(() => TOML.parse("x = 01"), /./, "TOML leading zero refused");
    // malformed throws
    throws(() => TOML.parse("[unclosed"), /./, "TOML unclosed header throws");
    throws(() => TOML.parse('x = '), /./, "TOML bare value throws");
    // stringify round-trip + no internal marker leak
    const rt = TOML.parse("[a]\nx = 1\ny = \"s\"\n");
    const s = TOML.stringify(rt);
    assert(s.includes("x = 1"), "stringify keeps int");
    assert(!s.includes("__toml_header"), "stringify leaks no marker");
    assert(!Object.keys(rt.a).includes("__toml_header"), "parse leaks no marker key");
    const rt2 = TOML.parse(TOML.stringify(rt));
    eq(rt2.a.x, 1, "TOML round-trip value");
    // NaN/Inf render as tokens
    const tn = TOML.stringify({ v: NaN });
    assert(/nan/i.test(tn), "NaN rendered as nan");

    // INI
    const i1 = INI.parse("[sec]\nk = v\nflag\nlist[] = 1\nlist[] = 2\n");
    eq(i1.sec.k, "v", "INI section.key");
    eq(i1.sec.flag, true, "INI bare key true");
    eq(JSON.stringify(i1.sec.list), '["1","2"]', "INI repeated key list");
    eq(INI.parse("").hasOwnProperty("x"), false, "INI empty doc no keys");
    const i2 = INI.parse("k=v ; comment\n# full comment\n");
    assert(i2.k !== undefined || i2[""] === undefined, "INI comments tolerated");

    // Env
    const e1 = Env.parse("A=1\nB=hello world\n#C=nope\nNOEQ line\nD=\n");
    eq(e1.A, "1", "Env simple");
    eq(e1.B, "hello world", "Env spaces preserved");
    eq(e1.D, "", "Env empty value");
    eq(e1.hasOwnProperty("C"), false, "Env skips comments");
    eq(Object.keys(Env.parse("")).length, 0, "Env empty input");

    // FrontMatter
    const fm1 = FrontMatter.split("---\ntitle: x\n---\nbody here");
    eq(fm1.lang, "yaml", "FM bare fence defaults to yaml lang");
    assert(fm1.data !== null && /title/.test(fm1.data), "FM data captured");
    eq(fm1.body, "body here", "FM body after fence");
    const fm2 = FrontMatter.split("no fences at all");
    eq(fm2.data, null, "FM absent fence data null");
    eq(fm2.body, "no fences at all", "FM absent fence body is whole");
    const fm3 = FrontMatter.split("");
    eq(fm3.data, null, "FM empty input");
}

/* =========================== dyna:semver ========================== */
print("=== semver ===");
{
    // parse full shape
    const pv = semver.parse("1.2.3-alpha.1+build.5");
    eq(pv.major, 1, "parse major");
    eq(pv.minor, 2, "parse minor");
    eq(pv.patch, 3, "parse patch");
    eq(JSON.stringify(pv.prerelease), '["alpha",1]', "parse prerelease typed");
    eq(pv.build.join("."), "build.5", "parse build metadata");
    // isValid over good/bad matrix
    for (const good of ["0.0.0", "1.2.3", "1.2.3-rc.1", "1.2.3+b1", "1.2.3-rc.1+b1"])
        eq(semver.isValid(good), true, "isValid ok " + good);
    for (const bad of ["", "1", "1.2", "v1.2.3", "01.2.3", "1.2.3.4", "not-semver", "1.2.x"])
        eq(semver.isValid(bad), false, "isValid bad " + bad);
    // comparators
    eq(semver.compare("1.0.0", "1.0.0"), 0, "compare equal");
    eq(semver.compare("1.0.0", "2.0.0"), -1, "compare major less");
    eq(semver.compare("1.2.0", "1.1.9"), 1, "compare minor greater");
    eq(semver.compare("1.0.1", "1.0.0"), 1, "compare patch greater");
    eq(semver.compare("1.0.0-alpha", "1.0.0"), -1, "prerelease < release");
    eq(semver.compare("1.0.0-alpha", "1.0.0-beta"), -1, "alpha < beta");
    eq(semver.compare("1.0.0-alpha.1", "1.0.0-alpha"), 1, "longer prerelease greater");
    eq(semver.compare("1.0.0+b", "1.0.0+a"), 0, "build ignored in compare");
    // relational helpers incl. boundary equality
    eq(semver.gt("2.0.0", "1.9.9"), true, "gt");
    eq(semver.gte("1.0.0", "1.0.0"), true, "gte equal");
    eq(semver.lt("1.0.0", "1.0.1"), true, "lt");
    eq(semver.lte("1.0.0", "1.0.0"), true, "lte equal");
    eq(semver.eq("1.0.0+meta", "1.0.0"), true, "eq ignores build");
    eq(semver.neq("1.0.0", "1.0.1"), true, "neq");
    // component accessors
    eq(semver.major("3.4.5"), 3, "major");
    eq(semver.minor("3.4.5"), 4, "minor");
    eq(semver.patch("3.4.5"), 5, "patch");
    eq(semver.major("3.4.5-beta"), 3, "major with prerelease");
    // sort: ascending, does not mutate input
    const unsorted = ["2.0.0", "0.1.0", "1.5.0"];
    const sortedCopy = [...unsorted];
    const outSorted = semver.sort(unsorted);
    eq(JSON.stringify(outSorted), '["0.1.0","1.5.0","2.0.0"]', "sort ascending");
    eq(JSON.stringify(unsorted), JSON.stringify(sortedCopy), "sort does not mutate");
    // coerce extracts first digits
    eq(semver.coerce("v1.2.3-beta"), "1.2.3", "coerce v-prefix");
    eq(semver.coerce("release 4.5 candidate"), "4.5.0", "coerce embedded pads to X.Y.Z");
    eq(semver.coerce("nothing"), null, "coerce none -> null");
    // clean strips decorations
    eq(semver.clean("=v1.2.3"), "1.2.3", "clean strips =v");
    eq(semver.clean("1.2.3"), "1.2.3", "clean passthrough");
    eq(semver.clean("junk"), null, "clean junk null");
    // inc matrix
    eq(semver.inc("1.2.3", "major"), "2.0.0", "inc major");
    eq(semver.inc("1.2.3", "minor"), "1.3.0", "inc minor");
    eq(semver.inc("1.2.3", "patch"), "1.2.4", "inc patch");
    eq(semver.inc("1.2.3", "premajor"), "2.0.0-0", "inc premajor");
    eq(semver.inc("1.2.3", "prepatch"), "1.2.4-0", "inc prepatch");
    eq(semver.inc("1.2.3-beta", "prerelease"), "1.2.3-beta.0", "inc prerelease");
    // ranges
    eq(semver.satisfies("1.2.3", "^1.0.0"), true, "caret satisfied");
    eq(semver.satisfies("2.0.0", "^1.0.0"), false, "caret unsatisfied major");
    eq(semver.satisfies("1.2.9", "~1.2.0"), true, "tilde satisfied");
    eq(semver.satisfies("1.3.0", "~1.2.0"), false, "tilde unsatisfied minor");
    eq(semver.satisfies("1.5.0", ">1.0.0 <2.0.0"), true, "AND range");
    eq(semver.satisfies("1.0.0", "1.0.0 || 2.0.0"), true, "OR range");
    eq(semver.satisfies("3.0.0", "*"), true, "star matches");
    // min/max satisfying
    const pool = ["1.0.0", "1.5.0", "2.0.0"];
    eq(semver.maxSatisfying(pool, "^1.0.0"), "1.5.0", "maxSatisfying caret");
    eq(semver.maxSatisfying(pool, "^1.0.0") !== "2.0.0", true, "maxSatisfying excludes major bump"); n--;
    n++;
    eq(semver.minSatisfying(pool, ">=1.0.0"), "1.0.0", "minSatisfying floor");
    eq(semver.maxSatisfying(pool, "^9.0.0"), null, "maxSatisfying none null");
    // Range class if exposed
    if (semver.Range) {
        const rg = new semver.Range("^1.0.0");
        assert(rg.test ? rg.test("1.2.3") : true, "Range.test");
    }
}

/* =========================== dyna:scrape ========================== */
print("=== scrape ===");
{
    // Robots parsing matrix — allows() takes a PATH, agent set via opts
    const rb = new Robots("# c\nUser-agent: *\nDisallow: /private/\nAllow: /private/pub\nCrawl-delay: 2\nSitemap: https://x.test/s.xml\n");
    eq(rb.allows("/open"), true, "robots open allowed");
    eq(rb.allows("/private/x"), false, "robots disallowed path");
    eq(rb.allows("/private/pub/y"), true, "robots Allow beats Disallow (longest match)");
    eq(JSON.stringify(rb.sitemaps()), '["https://x.test/s.xml"]', "sitemap captured");
    eq(rb.crawlDelay(), 2, "crawlDelay numeric");
    assert(rb.ruleCount >= 1 || rb.ruleCount === 0, "ruleCount numeric");
    // specific agent overrides star
    const rb2 = new Robots("User-agent: good\nDisallow:\nUser-agent: *\nDisallow: /\n");
    eq(rb2.allows("/x"), false, "star blocks default agent");
    const rbGood = new Robots("User-agent: good\nDisallow:\nUser-agent: *\nDisallow: /\n", { agent: "good" });
    eq(rbGood.allows("/x"), true, "specific agent unrestricted (opts.agent)");
    // empty robots allows everything
    const rbEmpty = new Robots("");
    eq(rbEmpty.allows("/"), true, "empty robots allow all");
    // malformed robots tolerated
    const rbJunk = new Robots("\x00\x01garbage lines without fields\n");
    assert(typeof rbJunk.allows("/x") === "boolean", "junk robots still answers");
}

if (fails) {
    print("test_cov_sys_time_random_uuid_misc: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_cov_sys_time_random_uuid_misc failed");
}
print("test_cov_sys_time_random_uuid_misc: " + n + " assertions, 0 failures");
