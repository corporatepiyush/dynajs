// covers: c:src/parser/parser.inc.c -- one-shot interrupt at every poll of module loading and async (TLA) module evaluation
// Module graphs written under .obj/isweep-mod/: a TLA leaf, a sync parent and a TLA parent of it, a rejecting TLA
// dependency with sync and TLA parents, a sync body that throws after a TLA dependency, and a missing module.
// The interrupt must end the run from dynamic import, static import, the evaluate/fulfil/reject callbacks of async
// modules and the resolve/reject calls of import() promises; no case may turn it into a rejection.
// Usage: test_module_isweep (ISWEEP_ONLY=<substring>, ISWEEP_VERBOSE=1); run from the repo root.
#include "../lib/dyn-isweep.h"

#include <sys/stat.h>

#define D "'./.obj/isweep-mod/"

static const char* const files[][2] = {
    { "leaf.mjs", "import * as os from 'os';\nawait new Promise((r) => os.setTimeout(r, 0));\nexport const v = 1;\n" },
    { "sync-parent.mjs", "import { v } from './leaf.mjs';\nexport const w = v + 1;\n" },
    { "tla-parent.mjs", "import { v } from './leaf.mjs';\nawait 0;\nexport const z = v + 2;\n" },
    { "two-parents.mjs", "import { w } from './sync-parent.mjs';\nimport { z } from './tla-parent.mjs';\nexport const s = w + z;\n" },
    { "rej.mjs", "import * as os from 'os';\nawait new Promise((r) => os.setTimeout(r, 0));\nthrow new Error('dep');\n" },
    { "rej-sync-parent.mjs", "import './rej.mjs';\nexport const q = 1;\n" },
    { "rej-tla-parent.mjs", "import './rej-sync-parent.mjs';\nawait 0;\nexport const q2 = 1;\n" },
    { "sync-throw.mjs", "import { v } from './leaf.mjs';\nthrow new Error('sync ' + v);\n" },
    { "plain.mjs", "export const p = [1, 2, 3].map((x) => x * 2);\n" },
};

static const char* const cases[][2] = {
    { "dynamic import of a TLA leaf", "const m = await import(" D "leaf.mjs'); if (m.v !== 1) leak('v'); ok();\n" },
    { "dynamic import of a sync parent of a TLA leaf", "const m = await import(" D "sync-parent.mjs'); if (m.w !== 2) leak('w'); ok();\n" },
    { "dynamic import of a TLA parent of a TLA leaf", "const m = await import(" D "tla-parent.mjs'); if (m.z !== 3) leak('z'); ok();\n" },
    { "sync and TLA parents of one leaf", "const m = await import(" D "two-parents.mjs'); if (m.s !== 5) leak('s'); ok();\n" },
    { "rejecting TLA dependency", "await import(" D "rej-tla-parent.mjs').then(() => leak('no error'), ok);\n"
                                  "await import(" D "rej-sync-parent.mjs').then(() => leak('no error'), ok);\n" },
    { "sync body throws after a TLA dependency", "await import(" D "sync-throw.mjs').then(() => leak('no error'), ok);\n" },
    { "missing module rejects", "await import(" D "missing.mjs').then(() => leak('no error'), ok);\n" },
    { "import with attributes", "await import(" D "plain.mjs', { with: { type: 'js' } }).then(ok, leak);\n"
                                "await import(" D "plain.mjs', { with: 1 }).then(() => leak('no error'), ok);\n" },
    { "static import of a TLA graph", "import { s } from " D "two-parents.mjs';\nif (s !== 5) leak('s'); ok();\n" },
    { "concurrent dynamic imports", "const [a, b] = await Promise.all([import(" D "tla-parent.mjs'), import(" D "sync-parent.mjs')]); if (a.z + b.w !== 5) leak('sum'); ok();\n" },
};

static int write_file(const char* dir, const char* name, const char* body)
{
    char path[256];
    FILE* f;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "w");
    if (!f)
        return -1;
    fputs(body, f);
    return fclose(f);
}

int main(void)
{
    size_t i;
    const char* dir = ".obj/isweep-mod";
    long max_n = getenv("ISWEEP_MAX") ? atol(getenv("ISWEEP_MAX")) : 20000;
    alarm(600);
    if (isweep_enter_workdir() != 0)
        return 1;
    mkdir(dir, 0755);
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (write_file(dir, files[i][0], files[i][1]) < 0) {
            printf("FAIL: cannot write %s/%s\n", dir, files[i][0]);
            return 1;
        }
    }
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        isweep_case(cases[i][0], cases[i][1], max_n);
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", dir, files[i][0]);
        unlink(path);
    }
    rmdir(dir);
    isweep_leave_workdir();
    return isweep_report("module_isweep");
}
