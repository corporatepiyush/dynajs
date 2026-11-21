#include "scrape-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCRAPE)

double fe_retry_wait(fe_t* f, int st, int attempt, const char* retry_after)
{
    double back;
    if (st != 0 && !httpk_retry_status(st, HTTPK_RETRY_SCRAPE))
        return -1;
    if (attempt >= f->retries)
        return -1;
    back = httpk_backoff_ms(f->min_delay_ms, 0, attempt, &f->rng);
    if (st != 0 && retry_after) {
        double asked = httpk_retry_after_ms(retry_after, strlen(retry_after),
            NULL, 0, (int64_t)time(NULL));
        if (asked > 0)
            back = asked;
    }
    return back > 60000.0 ? -1 : back;
}

int fe_redirect_step(JSContext* ctx, fe_t* f, char* cur, size_t cap, int st,
    const char* loc, int hop)
{
    char nxt[1024];
    httpk_redirect_t plan;
    const char* next = NULL;
    int rc;
    if (loc && fe_resolve(cur, loc, nxt, sizeof nxt) > 0)
        next = nxt;
    rc = httpk_redirect_plan(st, "GET", cur, next, hop, f->max_redirects,
        f->allow_insecure_downgrade ? HTTPK_REDIR_ALLOW_DOWNGRADE : 0u, &plan);
    switch (rc) {
    case HTTPK_REDIR_FOLLOW:
        if (strlen(nxt) + 1 > cap) {
            JS_ThrowRangeError(ctx, "Fetcher: url is too long");
            return -1;
        }
        snprintf(cur, cap, "%s", nxt);
        return 1;
    case HTTPK_REDIR_E_TOO_MANY:
        JS_ThrowRangeError(ctx, "Fetcher: more than %d redirects", f->max_redirects);
        return -1;
    case HTTPK_REDIR_E_DOWNGRADE:
        JS_ThrowRangeError(ctx,
            "Fetcher: redirect downgrades https to http (%.80s); pass "
            "allowInsecureDowngrade: true to follow it",
            nxt);
        return -1;
    case HTTPK_REDIR_E_SCHEME:
        JS_ThrowTypeError(ctx,
            "Fetcher: only http:// and https:// urls (got %.60s)", nxt);
        return -1;
    default:
        return 0;
    }
}

#endif
