/* Adapted from metadata-proxy: MDPX v1 codec and observer key files.
 * UDPPTT endpoints use structural decoding only; verification is for observers. */
#include "mdpx.h"
#include <sodium.h>
#include <string.h>
uint64_t get64(const unsigned char *p)  {
    uint64_t x=0;
    for(int i=0;i<8;i++) x=x*256+p[i];
    return x;
}
void put64(unsigned char *p,uint64_t x)  {
    for(int i=7;i>=0;i--)  {
        p[i]=(unsigned char)x;
        x>>=8;
    }
}
static unsigned get16(const unsigned char *p)  {
    return (unsigned)p[0]*256+p[1];
}
static void put16(unsigned char *p,size_t x)  {
    p[0]=(unsigned char)(x>>8);
    p[1]=(unsigned char)x;
}
int label_valid(const unsigned char *s,size_t n)  {
    if(!n||n>63)return 0;
    for(size_t i=0;i<n;i++) if(!((s[i]>='A'&&s[i]<='Z')||(s[i]>='a'&&s[i]<='z')||(s[i]>='0'&&s[i]<='9')||s[i]=='.'||s[i]=='_'||s[i]=='-'))return 0;
    return 1;
}
void key_id(unsigned char *id,const unsigned char *pk)  {
    unsigned char h[32];
    crypto_hash_sha256(h,pk,32);
    memcpy(id,h,8);
}
int decode(const unsigned char *b,size_t n,struct packet *p)  {
    if(n<116||n>CAP||memcmp(b,"MDPX",4)||b[4]!=1||b[5])return -1;
    size_t l=b[8],h=115+l,z=get16(b+9);
    if(l<1||l>63||h>n||!label_valid(b+51,l)||h!=get16(b+6)||z!=n-h)return -1;
    memset(p,0,sizeof(*p));
    p->raw=b;
    p->length=n;
    p->label_length=l;
    memcpy(p->label,b+51,l);
    p->payload=b+h;
    p->payload_length=z;
    p->signature=b+51+l;
    p->id=b+11;
    p->timestamp=get64(b+19);
    p->session=b+27;
    p->sequence=get64(b+43);
    return 0;
}
size_t message(struct workspace *w,const struct packet *p)  {
    size_t h=51+p->label_length;
    memcpy(w->message,"metadata-proxy/v1",17);
    memcpy(w->message+17,p->raw,h);
    memcpy(w->message+17+h,p->payload,p->payload_length);
    return 17+h+p->payload_length;
}
int verify(struct workspace *w,const struct packet *p,const unsigned char *pk)  {
    size_t n=message(w,p);
    return crypto_sign_verify_detached(p->signature,w->message,n,pk);
}
int encode(struct workspace *w,unsigned char *b,size_t *n,const unsigned char *payload,size_t z,const char *label,const unsigned char *pk,const unsigned char *sk,const unsigned char *sid,uint64_t ts,uint64_t seq)  {
    size_t l=strlen(label),h=115+l;
    if(!label_valid((const unsigned char *)label,l)||h>CAP||z>CAP-h)return -1;
    memcpy(b,"MDPX",4);
    b[4]=1;
    b[5]=0;
    put16(b+6,h);
    b[8]=(unsigned char)l;
    put16(b+9,z);
    key_id(b+11,pk);
    put64(b+19,ts);
    memcpy(b+27,sid,16);
    put64(b+43,seq);
    memcpy(b+51,label,l);
    memcpy(b+h,payload,z);
    *n=h+z;
    struct packet p;
    if(decode(b,*n,&p))return -1;
    size_t m=message(w,&p);
    return crypto_sign_detached(b+51+l,NULL,w->message,m,sk);
}
enum result authorize(struct workspace *w,const struct packet *p,const struct trust *t,const struct identity **who)  {
    int known=0;
    for(size_t i=0;i<t->count;i++)if(!memcmp(p->id,t->entries[i].id,8)) {
        known=1;
        if(!strcmp(p->label,t->entries[i].label)) {
            *who=&t->entries[i];
            return verify(w,p,(*who)->pk)?SIGNATURE:OK;
        }
    }
    return known?UNAUTHORIZED:UNKNOWN;
}
