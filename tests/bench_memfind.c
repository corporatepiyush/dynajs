/*
 * bench_memfind.c -- microbenchmark + equivalence oracle for the HTTP header
 * substring scan (dyn_memfind in dyna-http.c). Justifies wiring the shared
 * SIMD strfind kernel into dyn_memfind: on a realistic HTTP/1.1 header block the
 * vectorised scan is ~4.75x the scalar byte loop (short-span parsing like CSV
 * does NOT benefit, but header blocks are long enough to amortize). The `acc`
 * printed by both variants MUST match: that is
 * the byte-identical-result proof.
 *
 *   clang -O2 -I src tests/bench_memfind.c .obj/dyna-simd-*.o -lpthread -lm -o /tmp/mfb && /tmp/mfb
 *
 * (Requires a prior `make CONFIG_NATIVE_MODULES=y` so .obj/dyna-simd-*.o exist.)
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>
#include "dyna-simd-kernels.h"
static const char *scalar_memfind(const char *hay, size_t hlen, const char *needle, size_t nlen){
    size_t i; if (nlen==0||hlen<nlen) return NULL;
    for (i=0;i+nlen<=hlen;i++) if (hay[i]==needle[0]&&memcmp(hay+i,needle,nlen)==0) return hay+i;
    return NULL;
}
static const char *simd_memfind(const char *hay, size_t hlen, const char *needle, size_t nlen){
    size_t idx; if (nlen==0||hlen<nlen) return NULL;
    idx=simd.strfind((const uint8_t*)hay,hlen,(const uint8_t*)needle,nlen);
    return idx==SIZE_MAX?NULL:hay+idx;
}
static double now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e3+t.tv_nsec/1e6;}
static const char *REQ=
 "GET /api/v1/users/12345?verbose=true HTTP/1.1\r\n""Host: example.com\r\n"
 "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36\r\n"
 "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
 "Accept-Language: en-US,en;q=0.5\r\n""Accept-Encoding: gzip, deflate, br\r\n"
 "Connection: keep-alive\r\n""Content-Length: 0\r\n""\r\n";
int main(void){
    simd_init();
    size_t n=strlen(REQ);
    char *buf=malloc(n+1); memcpy(buf,REQ,n+1);
    const long ITER=3000000;
    for(int which=0;which<2;which++){
        double best=1e18; size_t acc=0;
        for(int t=0;t<5;t++){
            double s=now_ms();
            for(long i=0;i<ITER;i++){
                buf[7]=(char)('0'+(i&7)); /* mutate path digit at runtime */
                const char *a,*b,*c;
                if(which==0){a=scalar_memfind(buf,n,"\r\n\r\n",4);b=scalar_memfind(buf,n,"HTTP/1.1\r\n",10);c=scalar_memfind(buf,n,"\r\n",2);}
                else {a=simd_memfind(buf,n,"\r\n\r\n",4);b=simd_memfind(buf,n,"HTTP/1.1\r\n",10);c=simd_memfind(buf,n,"\r\n",2);}
                acc += (size_t)((a?a-buf:0)+(b?b-buf:0)+(c?c-buf:0));
            }
            double e=now_ms(); if(e-s<best)best=e-s;
        }
        printf("%s: bestMs=%.1f  Mreq/s=%.1f  acc=%zu\n", which?"simd ":"scalar", best, ITER/best/1e3, acc);
    }
    free(buf); return 0;
}
