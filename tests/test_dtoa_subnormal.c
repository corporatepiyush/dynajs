/* test_dtoa_subnormal.c -- js_atod against a correctly-rounded reference.
 *
 * The subnormal branch of js_atod truncates: `a = m >> (-e - 1021)`. That reads
 * like a rounding bug, and dtoa.c carries an `XXX: check rounding` comment a few
 * lines above -- but the comment is on the UNDERFLOW branch, and `m` reaches the
 * shift already rounded to the target precision by mul_pow_round_to_d(JS_RNDN).
 *
 * This pins that, because it is not obvious from reading and no other test
 * covers it: the differential in bench/NUMERIC_DTOA_WIDTH.md compares two limb
 * widths, which share any rounding bug, and oracle_dtoa.js round-trips through
 * js_dtoa, which never emits a halfway case.
 *
 * Reference is libc strtod, which is correctly rounded. Cases are actual
 * subnormal bit patterns AND the exact halfway point between each adjacent
 * pair, which is where truncation and round-to-nearest-even disagree.
 *
 * Proved to fire: XOR 1 into that shift's result and 6216 of 6219 cases fail.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dtoa.h"
int main(void){
    JSATODTempMem am; char s[512];
    long checked=0, bad=0;
    /* Walk actual subnormal bit patterns, print each exactly, and re-parse.
       Also probe the halfway points BETWEEN adjacent subnormals, which is where
       a truncating shift and round-to-nearest-even disagree. */
    for (uint64_t bits=1; bits<0x0010000000000000ULL; bits = bits<3000? bits+1 : (uint64_t)(bits*1.3)+1) {
        double d; memcpy(&d,&bits,8);
        snprintf(s,sizeof s,"%.*e",40,d);
        const char*np; double got=js_atod(s,&np,10,0,&am);
        double ref=strtod(s,NULL);
        checked++;
        if (memcmp(&got,&ref,8)) { if(bad<6) printf("  MISMATCH %s\n    js=%a libc=%a\n",s,got,ref); bad++; }
        /* halfway to the next subnormal */
        uint64_t nb=bits+1; double nd; memcpy(&nd,&nb,8);
        long double mid=((long double)d+(long double)nd)/2.0L;
        snprintf(s,sizeof s,"%.*Le",40,mid);
        got=js_atod(s,&np,10,0,&am); ref=strtod(s,NULL);
        checked++;
        if (memcmp(&got,&ref,8)) { if(bad<6) printf("  MISMATCH(mid) %s\n    js=%a libc=%a\n",s,got,ref); bad++; }
    }
    /* the underflow boundary: below the smallest subnormal */
    const char *edge[]={"2.4703282292062327e-324","2.4703282292062328e-324",
                        "2.47032822920623e-324","1e-323","4.9e-324","2e-324","1.5e-323"};
    for (size_t i=0;i<sizeof edge/sizeof*edge;i++){
        const char*np; double got=js_atod(edge[i],&np,10,0,&am), ref=strtod(edge[i],NULL);
        checked++;
        if (memcmp(&got,&ref,8)) { printf("  MISMATCH(edge) %s js=%a libc=%a\n",edge[i],got,ref); bad++; }
    }
    printf("#S subnormal parse vs libc strtod: %ld checked, %ld mismatches\n",checked,bad);
    return bad?1:0;
}
