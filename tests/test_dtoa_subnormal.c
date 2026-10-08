#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dtoa.h"
int main(void){
    JSATODTempMem am; char s[512];
    long checked=0, bad=0;
    for (uint64_t bits=1; bits<0x0010000000000000ULL; bits = bits<3000? bits+1 : (uint64_t)(bits*1.3)+1) {
        double d; memcpy(&d,&bits,8);
        snprintf(s,sizeof s,"%.*e",40,d);
        const char*np; double got=js_atod(s,&np,10,0,&am);
        double ref=strtod(s,NULL);
        checked++;
        if (memcmp(&got,&ref,8)) { if(bad<6) printf("  MISMATCH %s\n    js=%a libc=%a\n",s,got,ref); bad++; }
        uint64_t nb=bits+1; double nd; memcpy(&nd,&nb,8);
        long double mid=((long double)d+(long double)nd)/2.0L;
        snprintf(s,sizeof s,"%.*Le",40,mid);
        got=js_atod(s,&np,10,0,&am); ref=strtod(s,NULL);
        checked++;
        if (memcmp(&got,&ref,8)) { if(bad<6) printf("  MISMATCH(mid) %s\n    js=%a libc=%a\n",s,got,ref); bad++; }
    }
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
