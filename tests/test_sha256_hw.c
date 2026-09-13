/* test_sha256_hw.c -- the hardware SHA-256 path against FIPS 180-4 and against
 * the scalar compression function.
 *
 * dyn_sha256 uses the ARMv8 SHA2 instructions where the target has them and a
 * scalar compression function otherwise. The instruction sequence is easy to
 * get subtly wrong -- the K addend must use the message vector BEFORE su0
 * advances it, and the four message vectors rotate -- and a wrong order still
 * produces a plausible-looking 32-byte digest. So this checks two things a
 * self-consistent test cannot: the published vectors, and every length 0..1024
 * folded into one hash so a single mis-compressed block shows.
 *
 * Build with -DDYN_SHA256_NO_HW=1 for the scalar control; the two must agree.
 *
 *   make test-sha256-hw
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
void dyn_sha256(const uint8_t*, size_t, uint8_t*);
static void hex(const uint8_t*d,char*o){for(int i=0;i<32;i++)sprintf(o+2*i,"%02x",d[i]);}
int main(void){
    uint8_t d[32]; char h[65];
    /* FIPS 180-4 known answers */
    dyn_sha256((const uint8_t*)"abc",3,d); hex(d,h);
    int bad = strcmp(h,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")!=0;
    if(bad) printf("  FAIL abc: %s\n",h);
    dyn_sha256((const uint8_t*)"",0,d); hex(d,h);
    if(strcmp(h,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")){printf("  FAIL empty: %s\n",h);bad++;}
    const char *m="abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    dyn_sha256((const uint8_t*)m,strlen(m),d); hex(d,h);
    if(strcmp(h,"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")){printf("  FAIL 448bit: %s\n",h);bad++;}
    /* every length 0..1024 folded into one hash, so one wrong block shows */
    uint8_t buf[1100]; for(int i=0;i<1100;i++) buf[i]=(uint8_t)(i*167u+13u);
    uint32_t acc=2166136261u;
    for(int n=0;n<=1024;n++){ dyn_sha256(buf,n,d); for(int i=0;i<32;i++){acc^=d[i];acc*=16777619u;} }
    printf("  KAT failures=%d  length-sweep hash=%08x\n",bad,acc);
    return bad!=0;
}
