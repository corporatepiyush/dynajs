/* test_file_platform.js — dataDir/configDir/cacheDir and their Site
 * variants (plan 3.8). Lookup MUST NOT create a directory (XDG spec) and
 * env override wins over defaults. Runs under the CURRENT OS and gates
 * the other branches loudly: an unrun branch is an unverified claim.
 * A lookup that "accepts everything" (ignores env / always creates)
 * fails the env-override and no-create assertions below.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_file_platform.js
 */
import { dataDir, configDir, cacheDir, dataDirSite, configDirSite, cacheDirSite,
         Path, exists, makeTempDir, removeAll } from "dyna:file";
import { setEnv, getEnv } from "dyna:sys";
import * as os from "os";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throws(fn, m) { let t = false; try { fn(); } catch (e) { t = true; } assert(t, m); }

const plat = os.platform;

/* ------------------------------------------------ macOS (current OS) */
if (plat === "darwin") {
    const tmp = String(makeTempDir("pdir"));
    const savedHome = getEnv("HOME");
    try {
        setEnv("HOME", tmp);
        eq(String(dataDir()), tmp + "/Library/Application Support", "dataDir on macOS");
        eq(String(configDir()), tmp + "/Library/Preferences", "configDir on macOS");
        eq(String(cacheDir()), tmp + "/Library/Caches", "cacheDir on macOS");
        eq(String(dataDirSite()), "/Library/Application Support", "dataDirSite on macOS");
        eq(String(configDirSite()), "/Library/Preferences", "configDirSite on macOS");
        eq(String(cacheDirSite()), "/Library/Caches", "cacheDirSite on macOS");
        /* app segment is appended */
        eq(String(dataDir("myapp")), tmp + "/Library/Application Support/myapp",
           "dataDir(app) appends one segment");
        /* XDG vars are NOT consulted on macOS (per plan): setting one must
         * not move the answer. */
        setEnv("XDG_DATA_HOME", tmp + "/xdg");
        eq(String(dataDir()), tmp + "/Library/Application Support",
           "macOS ignores XDG_DATA_HOME");
        setEnv("XDG_DATA_HOME", "");
        /* LOOKUP MUST NOT CREATE THE DIRECTORY (XDG spec) */
        const missing = tmp + "/no-such-home";
        setEnv("HOME", missing);
        eq(String(dataDir()), missing + "/Library/Application Support",
           "dataDir tracks HOME");
        assert(!exists(new Path(String(dataDir()))),
               "dataDir lookup did NOT create " + String(dataDir()));
        assert(!exists(new Path(missing)), "nor the base " + missing);
    } finally {
        setEnv("HOME", savedHome === undefined ? "" : savedHome);
        setEnv("XDG_DATA_HOME", "");
        removeAll(new Path(tmp));
    }
    /* refusals (OS-independent) */
    throws(() => dataDir(42), "dataDir(42) refuses a non-string app");
    throws(() => dataDir(""), "empty app refuses");
    throws(() => dataDir("a/b"), "'/' in app refuses");
    throws(() => dataDir("a\\b"), "'\\' in app refuses");
    throws(() => dataDir(".."), "'..' refuses");
    throws(() => dataDir("\u0000"), "NUL in app refuses");
    print("  macOS branch ran");
} else if (plat === "linux") {
    /* ------------------------------------------------ Linux (XDG) -- */
    const tmp = String(makeTempDir("pdir"));
    const savedHome = getEnv("HOME");
    try {
        setEnv("HOME", tmp);
        setEnv("XDG_DATA_HOME", "");
        setEnv("XDG_CONFIG_HOME", "");
        setEnv("XDG_CACHE_HOME", "");
        setEnv("XDG_DATA_DIRS", "");
        setEnv("XDG_CONFIG_DIRS", "");
        /* defaults when env is unset/empty */
        eq(String(dataDir()), tmp + "/.local/share", "default dataDir");
        eq(String(configDir()), tmp + "/.config", "default configDir");
        eq(String(cacheDir()), tmp + "/.cache", "default cacheDir");
        eq(String(dataDirSite()), "/usr/local/share", "default site data");
        eq(String(configDirSite()), "/etc/xdg", "default site config");
        eq(String(cacheDirSite()), "/var/cache", "default site cache");
        /* env override wins */
        setEnv("XDG_DATA_HOME", tmp + "/xdg-data");
        eq(String(dataDir()), tmp + "/xdg-data", "XDG_DATA_HOME wins");
        setEnv("XDG_CONFIG_HOME", tmp + "/xdg-config");
        eq(String(configDir()), tmp + "/xdg-config", "XDG_CONFIG_HOME wins");
        setEnv("XDG_CACHE_HOME", tmp + "/xdg-cache");
        eq(String(cacheDir()), tmp + "/xdg-cache", "XDG_CACHE_HOME wins");
        /* RELATIVE env value is invalid per the spec: ignored -> default */
        setEnv("XDG_DATA_HOME", "relative/path");
        eq(String(dataDir()), tmp + "/.local/share", "relative XDG_DATA_HOME ignored");
        setEnv("XDG_DATA_HOME", "");
        /* site list: first entry wins */
        setEnv("XDG_DATA_DIRS", tmp + "/one:" + tmp + "/two");
        eq(String(dataDirSite()), tmp + "/one", "XDG_DATA_DIRS first entry");
        /* LOOKUP MUST NOT CREATE */
        const missing = tmp + "/no-such-dir";
        setEnv("XDG_DATA_HOME", missing);
        eq(String(dataDir()), missing, "XDG_DATA_HOME wins");
        assert(!exists(new Path(missing)),
               "dataDir lookup did NOT create " + missing);
    } finally {
        setEnv("HOME", savedHome === undefined ? "" : savedHome);
        for (const v of ["XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                         "XDG_DATA_DIRS", "XDG_CONFIG_DIRS"])
            setEnv(v, "");
        removeAll(new Path(tmp));
    }
    throws(() => dataDir(42), "dataDir(42) refuses a non-string app");
    throws(() => dataDir(""), "empty app refuses");
    throws(() => dataDir("a/b"), "'/' in app refuses");
    throws(() => dataDir(".."), "'..' refuses");
    print("  linux branch ran");
} else {
    print("test_file_platform: SKIP on " + plat + " (runs on darwin/linux)");
}

if (fails) {
    print("test_file_platform: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_file_platform failed");
}
print("test_file_platform: " + n + " assertions, 0 failures");
