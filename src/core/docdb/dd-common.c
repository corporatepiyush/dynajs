#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "dd.h"
#include "../dyn-hash.h"

int dd_fail(dd_err* e, int code, const char* fmt, ...)
{
    va_list ap;
    if (!e)
        return -1;
    e->code = code;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
    return -1;
}

const char* dd_code_name(int code)
{
    switch (code) {
    case DD_E_INTERNAL:
        return "InternalError";
    case DD_E_BADVALUE:
        return "BadValue";
    case DD_E_FAILEDTOPARSE:
        return "FailedToParse";
    case DD_E_TYPEMISMATCH:
        return "TypeMismatch";
    case DD_E_NAMESPACENOTFOUND:
        return "NamespaceNotFound";
    case DD_E_INDEXNOTFOUND:
        return "IndexNotFound";
    case DD_E_PATHNOTVIABLE:
        return "PathNotViable";
    case DD_E_CONFLICTINGUPDATE:
        return "ConflictingUpdateOperators";
    case DD_E_IMMUTABLEFIELD:
        return "ImmutableField";
    case DD_E_INVALIDOPTIONS:
        return "InvalidOptions";
    case DD_E_INDEXOPTIONSCONFLICT:
        return "IndexOptionsConflict";
    case DD_E_INDEXKEYSPECSCONFLICT:
        return "IndexKeySpecsConflict";
    case DD_E_DOCVALIDATION:
        return "DocumentValidationFailure";
    case DD_E_PARALLELARRAYS:
        return "CannotIndexParallelArrays";
    case DD_E_HISTORYLOST:
        return "ChangeStreamHistoryLost";
    case DD_E_DUPKEY:
        return "DuplicateKey";
    case DD_E_NSEXISTS:
        return "NamespaceExists";
    case 31250:
        return "PathCollision";
    case 31254:
        return "ProjectionMixed";
    case DD_E_OOM:
        return "ExceededMemoryLimit";
    case DD_E_BADSNAPSHOT:
        return "InvalidSnapshotFile";
    case DD_E_IO:
        return "SnapshotIOError";
    case DD_E_TOOLARGE:
        return "BSONObjectTooLarge";
    case DD_E_REGEXTIMEOUT:
        return "RegexTimeout";
    }
    return "UnknownError";
}

uint64_t dd_hash_bytes(const uint8_t* p, size_t n)
{
    return dyn_xxh64(p, n, 0x9E3779B97F4A7C15ull);
}

int dd_idvec_push(dd_idvec* v, uint32_t x)
{
    if (v->n == v->cap) {
        uint32_t nc = v->cap ? v->cap * 2 : 64;
        uint32_t* np = realloc(v->ids, (size_t)nc * sizeof *np);
        if (!np)
            return -1;
        v->ids = np;
        v->cap = nc;
    }
    v->ids[v->n++] = x;
    return 0;
}

void dd_idvec_free(dd_idvec* v)
{
    free(v->ids);
    v->ids = NULL;
    v->n = v->cap = 0;
}
