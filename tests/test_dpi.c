#include "mdpx.h"
#include <sodium.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static struct workspace w;
static unsigned char b[CAP],copy[CAP],payload[CAP],pk[32],sk[64],sid[16];
static void build(size_t z,const char *label,uint64_t seq,uint64_t ts,size_t *n) {
    assert(!encode(&w,b,n,payload,z,label,pk,sk,sid,ts,seq));
}
int main(void) {
    assert(sodium_init()>=0);
    unsigned char seed[32];
    for(int i=0;i<32;i++)seed[i]=(unsigned char)i;
    for(int i=0;i<16;i++)sid[i]=(unsigned char)(0xa0+i);
    crypto_sign_seed_keypair(pk,sk,seed);
    for(size_t i=0;i<CAP;i++)payload[i]=(unsigned char)i;
    size_t n;
    struct packet p;
    char label[64];
    memset(label,'A',63);
    label[63]=0;
    size_t sizes[]= {
        0,4,160,1024,1079,CAP-178
    };
    for(size_t i=0;i<sizeof sizes/sizeof *sizes;i++) {
        build(sizes[i],label,0,1000,&n);
        assert(!decode(b,n,&p));
        assert(!verify(&w,&p,pk));
        assert(p.payload_length==sizes[i]&&!memcmp(p.payload,payload,sizes[i]));
    }
    build(4,"HOST-A",UINT64_C(0x0102030405060708),UINT64_C(0x0000018bcfe56800),&n);
    assert(n==125&&b[6]==0&&b[7]==121&&b[8]==6&&b[9]==0&&b[10]==4);
    assert(p.label_length==63);
    assert(!decode(b,n,&p));
    assert(p.sequence==UINT64_C(0x0102030405060708));
    assert(!memcmp(b+27,sid,16));
    assert(!memcmp(b+121,payload,4));
    FILE *fixture=fopen("tests/dpi_fixture.hex","r");
    assert(fixture);
    char hex[252];
    assert(fgets(hex,sizeof hex,fixture));
    assert(!fclose(fixture));
    unsigned char expected[125];
    assert(!sodium_hex2bin(expected,sizeof expected,hex,250,NULL,NULL,NULL));
    assert(!memcmp(expected,b,125));
    memcpy(copy,b,n);
    for(size_t i=0;i<n;i++)assert(decode(b,i,&p));
    for(size_t i=0;i<n;i++) {
        memcpy(b,copy,n);
        b[i]^=0x80;
        if(!decode(b,n,&p))assert(verify(&w,&p,pk));
    }
    memcpy(b,copy,n);
    b[n]=0;
    assert(decode(b,n+1,&p));
    assert(decode(b,CAP+1,&p));
    /* Malicious label lengths must be rejected before reading label bytes. */
    for (int l = 0; l < 256; l++) {
        memcpy(b, copy, n); b[8] = (unsigned char)l;
        if (l != 6) assert(decode(b, n, &p));
    }
    puts("DPI fixture, boundary round trips, tamper and truncation checks passed");
    return 0;
}
