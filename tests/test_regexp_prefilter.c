/* test_regexp_prefilter.c -- the start-position prefilter against brute force.
 *
 * WHY THIS EXISTS AND THE FUZZER DOES NOT REPLACE IT.
 *
 * tests/oracle_regexp_fuzz.js is a random-pattern oracle and it cannot reach
 * this code. Measured, twice: an off-by-one injected into the 16-bit literal
 * scan produced a BYTE-IDENTICAL result hash there -- from the four identities
 * and from the hash alike -- because reaching RE_PF_LIT16 needs a pattern that
 * begins with two or more literal units AND a subject at least RE_PF_MIN_LEN
 * long AND a match in the final units, and random generation almost never
 * produces that conjunction. (The subject-length and wide-subject gaps in that
 * fuzzer were real and are fixed; this conjunction is simply not something a
 * random generator finds.)
 *
 * So the prefilter gets a systematic differential instead: every needle length
 * against every subject length, narrow and wide, compared to a brute-force
 * search. That injection produces 1340 mismatches here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cutils.h"
#include "libregexp.h"
BOOL lre_check_stack_overflow(void*o,size_t n){(void)o;(void)n;return FALSE;}
void *lre_realloc(void*o,void*p,size_t n){(void)o;if(!n){free(p);return NULL;}return realloc(p,n);}
int lre_check_timeout(void*o){(void)o;return 0;}

static int fails=0, cases=0;

/* brute force: first index where needle occurs in hay (both arrays of uint32) */
static int brute(const uint32_t*hay,int hl,const uint32_t*nd,int nl){
    for(int i=0;i+nl<=hl;i++){ int j=0; while(j<nl&&hay[i+j]==nd[j])j++; if(j==nl) return i; }
    return -1;
}
static void check(const uint32_t*hay,int hl,const uint32_t*nd,int nl,int wide){
    char pat[256]; int p=0;
    for(int i=0;i<nl;i++) p+=sprintf(pat+p,"\\u%04X",nd[i]);
    pat[p]=0;
    int bclen; char err[128];
    uint8_t*bc=lre_compile(&bclen,err,sizeof(err),pat,strlen(pat),0,NULL);
    if(!bc){ printf("compile fail %s: %s\n",pat,err); fails++; return; }
    uint8_t*sub;
    if(wide){ uint16_t*b=malloc((hl?hl:1)*2); for(int i=0;i<hl;i++)b[i]=(uint16_t)hay[i]; sub=(uint8_t*)b; }
    else    { uint8_t*b=malloc(hl?hl:1);      for(int i=0;i<hl;i++)b[i]=(uint8_t)hay[i];  sub=b; }
    uint8_t**cap=malloc(sizeof(*cap)*lre_get_alloc_count(bc));
    int ret=lre_exec(cap,bc,sub,0,hl,wide,NULL);
    int want=brute(hay,hl,nd,nl);
    int got = ret==1 ? (int)((cap[0]-sub)>>wide) : -1;
    cases++;
    if((want<0) != (ret!=1) || (want>=0 && got!=want)){
        printf("MISMATCH wide=%d hl=%d nl=%d want=%d got=%d ret=%d pat=%s\n",
               wide,hl,nl,want,got,ret,pat); fails++;
    }
    free(cap);free(sub);free(bc);
}
/* Character-class prefilters (RE_PF_SET and RE_PF_BITMAP) against brute force.
   The literal cases below cover neither: a class takes a different build path
   and a different scan, and the bitmap form is what \d, [a-z] and \w use. */
static void check_class(const char *cls, int (*member)(int), int wide)
{
    char pat[64];
    snprintf(pat, sizeof pat, "%sZQX", cls);
    int bclen; char err[128];
    uint8_t *bc = lre_compile(&bclen, err, sizeof err, pat, strlen(pat), 0, NULL);
    if (!bc) { printf("compile %s: %s\n", pat, err); fails++; return; }
    for (int hl = 0; hl <= 200; hl++) {
        uint32_t hay[256];
        for (int i = 0; i < hl; i++) hay[i] = (uint32_t)(" azAZ09_-\t~"[i % 12]);
        /* brute force: first index where the class matches and "ZQX" follows */
        int want = -1;
        for (int i = 0; i + 4 <= hl; i++)
            if (member((int)hay[i]) && hay[i+1]=='Z' && hay[i+2]=='Q' && hay[i+3]=='X') { want = i; break; }
        /* plant a real match near the end so the scan has something to find */
        if (want < 0 && hl >= 8) {
            hay[hl-4] = 'a'; hay[hl-3]='Z'; hay[hl-2]='Q'; hay[hl-1]='X';
            want = member('a') ? hl-4 : -1;
        }
        uint8_t *sub; 
        if (wide) { uint16_t *b = malloc((hl?hl:1)*2); for(int i=0;i<hl;i++) b[i]=(uint16_t)hay[i]; sub=(uint8_t*)b; }
        else      { uint8_t *b = malloc(hl?hl:1);      for(int i=0;i<hl;i++) b[i]=(uint8_t)hay[i];  sub=b; }
        uint8_t **cap = malloc(sizeof(*cap) * lre_get_alloc_count(bc));
        int ret = lre_exec(cap, bc, sub, 0, hl, wide, NULL);
        int got = ret == 1 ? (int)((cap[0]-sub) >> wide) : -1;
        cases++;
        if (got != want) { if (fails < 8) printf("CLASS MISMATCH %s wide=%d hl=%d want=%d got=%d\n", cls, wide, hl, want, got); fails++; }
        free(cap); free(sub);
    }
    free(bc);
}
/* Leading alternation. This path DECIDES WHICH POSITIONS ARE TRIED, so an
   under-approximated first-byte set silently loses matches; brute force over
   every alternative is the only way to see that. Includes shapes that must be
   DECLINED (a branch matching empty, a leading dot) -- declining is correct,
   answering wrongly is not. */
static void check_alt(const char *pat, const char *alts[], int nalts)
{
    int bclen; char err[128];
    uint8_t *bc = lre_compile(&bclen, err, sizeof err, pat, strlen(pat), 0, NULL);
    if (!bc) { printf("compile %s: %s\n", pat, err); fails++; return; }
    static const char alpha[] = "abcdxyz019_ ";
    for (int hl = 0; hl <= 160; hl++) {
        char hay[200];
        for (int i = 0; i < hl; i++) hay[i] = alpha[(i * 7 + hl) % 12];
        /* plant one alternative near the end for half the lengths */
        if (hl > 20 && (hl & 1)) {
            const char *a = alts[hl % nalts];
            size_t al = strlen(a);
            if ((size_t)hl > al + 2) memcpy(hay + hl - al - 1, a, al);
        }
        int want = -1;
        for (int i = 0; i < hl && want < 0; i++)
            for (int k = 0; k < nalts; k++) {
                size_t al = strlen(alts[k]);
                if ((size_t)(hl - i) >= al && memcmp(hay + i, alts[k], al) == 0) { want = i; break; }
            }
        uint8_t **cap = malloc(sizeof(*cap) * lre_get_alloc_count(bc));
        int ret = lre_exec(cap, bc, (uint8_t *)hay, 0, hl, 0, NULL);
        int got = ret == 1 ? (int)((cap[0] - (uint8_t *)hay)) : -1;
        cases++;
        if (got != want) { if (fails < 8) printf("ALT MISMATCH %s hl=%d want=%d got=%d\n", pat, hl, want, got); fails++; }
        free(cap);
    }
    free(bc);
}
static int m_digit(int c){ return c>='0'&&c<='9'; }
static int m_lower(int c){ return c>='a'&&c<='z'; }
static int m_word(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'; }
static int m_pair(int c){ return c=='a'||c=='Z'; }

int main(void){
    uint32_t hay[80], nd[20];
    /* alphabet chosen so the FIRST needle unit repeats often -> drives the
       find-then-verify loop through many false positives */
    for(int wide=0;wide<2;wide++){
      for(int hl=0;hl<=64;hl++){
        for(int nl=1;nl<=(hl<17?hl+2:17);nl++){
          for(int variant=0;variant<3;variant++){
            for(int i=0;i<hl;i++){
                uint32_t base = wide ? 0x100 : 0x41;   /* wide: above Latin-1 */
                hay[i] = base + ((i*variant+i/3) % 3);
            }
            for(int j=0;j<nl;j++){
                uint32_t base = wide ? 0x100 : 0x41;
                nd[j] = base + ((j+variant) % 3);
            }
            check(hay,hl,nd,nl,wide);
            /* force a guaranteed match at the very END */
            if(hl>=nl){ for(int j=0;j<nl;j++) hay[hl-nl+j]=nd[j]; check(hay,hl,nd,nl,wide); }
            /* and at index 0 */
            if(hl>=nl){ for(int j=0;j<nl;j++) hay[j]=nd[j]; check(hay,hl,nd,nl,wide); }
          }
        }
      }
    }
    check_class("[0-9]", m_digit, 0); check_class("[0-9]", m_digit, 1);
    check_class("[a-z]", m_lower, 0); check_class("[a-z]", m_lower, 1);
    check_class("[a-zA-Z0-9_]", m_word, 0); check_class("[a-zA-Z0-9_]", m_word, 1);
    check_class("[aZ]", m_pair, 0);   /* <=8 values: the RE_PF_SET path */
    { const char *a[] = {"ab","cd"};        check_alt("(ab|cd)", a, 2); }
    { const char *a[] = {"ab","cd"};        check_alt("(?:ab|cd)", a, 2); }
    { const char *a[] = {"a","b","c"};      check_alt("(a|b|c)", a, 3); }
    { const char *a[] = {"xy","z9","01"};   check_alt("(xy|z9|01)", a, 3); }
    { const char *a[] = {"ax","bx","cx"};   check_alt("((a|b)x|cx)", a, 3); }
    { const char *a[] = {"0z","9z","_z"};   check_alt("([0-9_]z)", a, 3); }
    /* Shapes the first-byte walk must DECLINE rather than mis-filter: a branch
       that can match empty admits every position. Spelled out as the full set
       of alternatives -- an earlier version listed only "a" for (a?|b)c, which
       is not what that pattern matches, and the 310 "failures" it reported were
       the reference being wrong, not the engine. */
    { const char *a[] = {"ac","c","bc"};    check_alt("(a?|b)c", a, 3); }
    { const char *a[] = {"xb"};             check_alt("(x|xy)b", a, 1); }
    printf("#PF cases=%d fails=%d\n",cases,fails);
    return fails?1:0;
}
