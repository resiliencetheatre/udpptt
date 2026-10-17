/* Adapted from metadata-proxy: MDPX v1 codec and observer key files.
 * UDPPTT endpoints use structural decoding only; verification is for observers. */
#include "mdpx.h"
#include <sodium.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
static int unhex(const char *s,unsigned char *b) {
    for(int i=0;i<64;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return -1;
    return sodium_hex2bin(b,32,s,64,NULL,NULL,NULL);
}
int secret_load(const char *path,unsigned char *pk,unsigned char *sk) {
    unsigned char seed[32];
    char b[67]= {
        0
    };
    FILE *f=fopen(path,"rb");
    if(!f)return -1;
    size_t n=fread(b,1,sizeof b,f);
    int bad=ferror(f);
    if(fclose(f))bad=1;
    if(n!=65||b[64]!='\n'||unhex(b,seed))bad=1;
    if(!bad)crypto_sign_seed_keypair(pk,sk,seed);
    sodium_memzero(seed,sizeof seed);
    sodium_memzero(b,sizeof b);
    return bad?-1:0;
}
static int writekey(const char *path,const unsigned char *key,int mode) {
    char b[66];
    sodium_bin2hex(b,sizeof b,key,32);
    b[64]='\n';
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,mode);
    if(fd<0) {
        sodium_memzero(b,sizeof b);
        return -1;
    }
    size_t done=0;
    int bad=0;
    while(done<65) {
        ssize_t n=write(fd,b+done,65-done);
        if(n<0&&errno==EINTR)continue;
        if(n<=0) {
            bad=1;
            break;
        }
        done+=(size_t)n;
    }
    if(close(fd))bad=1;
    sodium_memzero(b,sizeof b);
    return bad?-1:0;
}
int keygen(const char *secret,const char *public) {
    unsigned char seed[32],pk[32],sk[64];
    randombytes_buf(seed,32);
    crypto_sign_seed_keypair(pk,sk,seed);
    int rc=writekey(secret,seed,0600);
    if(!rc)rc=writekey(public,pk,0644);
    sodium_memzero(seed,sizeof seed);
    sodium_memzero(sk,sizeof sk);
    return rc;
}
/* Read a bounded text line without accepting hidden bytes after a NUL. */
static int trust_line(FILE *f, char *line, size_t capacity) {
    size_t n = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == 0 || n == capacity - 1) return -1;
        line[n++] = (char)c;
        if (c == '\n') break;
    }
    if (ferror(f)) return -1;
    line[n] = 0;
    return n != 0;
}
int trust_load(const char *path,struct trust *t) {
    FILE *f=fopen(path,"r");
    if(!f)return -1;
    char line[512];
    t->count=0;
    int bad=0;
    int line_result;
    while ((line_result = trust_line(f, line, sizeof line)) > 0) {
        char *p=line;
        while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n')p++;
        if(!*p||*p=='#')continue;
        char hex[66],label[65],extra[2];
        if(sscanf(p,"%65s %64s %1s",hex,label,extra)!=2||strlen(hex)!=64||!label_valid((unsigned char *)label,strlen(label))||t->count==TRUST_CAP) {
            bad=1;
            break;
        }
        struct identity *e=&t->entries[t->count];
        if(unhex(hex,e->pk)) {
            bad=1;
            break;
        }
        key_id(e->id,e->pk);
        strcpy(e->label,label);
        for(size_t i=0;i<t->count;i++)if(!memcmp(e->id,t->entries[i].id,8)&&memcmp(e->pk,t->entries[i].pk,32))bad=1;
        if(bad)break;
        t->count++;
    }
    if(line_result < 0 || ferror(f))bad=1;
    if(fclose(f))bad=1;
    return bad?-1:0;
}
