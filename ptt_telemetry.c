/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ptt_telemetry.h"
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <limits.h>
#define PI 3.14159265358979323846
static const uint8_t sync_word[16] = {0,15,3,11,6,1,13,8,4,14,2,10,7,12,5,9};
static float basis[16][11][2][320];
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void init_basis(void) {
    for (int s=0;s<16;s++) for (int h=2;h<=12;h++) for (int n=0;n<320;n++) {
        double a=2*PI*(100+10*s)*h*n/16000.;
        basis[s][h-2][0][n]=(float)cos(a);
        basis[s][h-2][1][n]=(float)sin(a);
    }
}
int tm_id(char out[6], const char *in) {
    if (strlen(in)!=5) return 0;
    for (int i=0;i<5;i++) {
        char c=in[i]; if (c>='a' && c<='z') c-=32;
        if (c<'A'||c>'Z') return 0;
        out[i]=c;
    }
    out[5]=0; return 1;
}
static uint16_t crc16(const uint8_t *p, int n) {
    uint16_t c=0xffff;
    for (int i=0;i<n;i++) { c^=(uint16_t)p[i]<<8; for(int b=0;b<8;b++) c=(c<<1)^((c&0x8000)?0x1021:0); }
    return c;
}
static void put32(uint8_t *p,uint32_t v) { for(int i=3;i>=0;i--) {p[i]=(uint8_t)v;v>>=8;} }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
static unsigned parity(unsigned x) { x^=x>>4;x^=x>>2;x^=x>>1;return x&1; }
int tm_build(tm_burst *b,const tm_record *r) {
    uint8_t raw[18]={0}, coded[300]={0}; char id[6];
    if(!tm_id(id,r->id)||r->gps<0||r->gps>1) return 0;
    if(r->gps && (r->latitude < -9000000 || r->latitude > 9000000 || r->longitude < -18000000 || r->longitude > 18000000)) return 0;
    unsigned v=0;for(int i=0;i<5;i++) v=v*26+(unsigned)(id[i]-'A');
    raw[0]=0x10|r->gps;raw[1]=v>>16;raw[2]=v>>8;raw[3]=v;
    raw[4]=r->sequence>>8;raw[5]=r->sequence;
    int len=r->gps?18:8;
    if(r->gps) {put32(raw+6,(uint32_t)r->latitude);put32(raw+10,(uint32_t)r->longitude);raw[14]=r->age>>8;raw[15]=r->age;}
    uint16_t crc=crc16(raw,len-2);raw[len-2]=crc>>8;raw[len-1]=crc;
    int bits=len*8+6, n=bits*2;unsigned state=0;
    for(int i=0;i<bits;i++) {
        unsigned bit=i<len*8?(raw[i/8]>>(7-i%8))&1:0;
        state=((state<<1)|bit)&127;
        coded[2*i]=parity(state&0171);coded[2*i+1]=parity(state&0133);
    }
    memset(b,0,sizeof(*b));
    /* 200 ms acquisition, 320 ms sync, coded payload, 80 ms ready chirp,
     * 20 ms fade, 100 ms silence. Symbols 16/17/18 designate cue/fade/silence. */
    for(int i=0;i<10;i++) b->symbols[b->frames++]=i&1?15:0;
    for(int i=0;i<16;i++) b->symbols[b->frames++]=sync_word[i];
    /* Transpose four rows. Adjacent bits lost in one audio symbol are separated. */
    for(int i=0;i<n/4;i++) {unsigned sym=0;for(int j=0;j<4;j++) sym=(sym<<1)|coded[j*(n/4)+i];b->symbols[b->frames++]=sym;}
    for(int i=0;i<4;i++) b->symbols[b->frames++]=16;
    b->symbols[b->frames++]=17;
    for(int i=0;i<5;i++) b->symbols[b->frames++]=18;
    return 1;
}
void tm_frame(const tm_burst *b,unsigned frame,int16_t pcm[TM_FRAME]) {
    memset(pcm,0,TM_FRAME*sizeof(*pcm));if(frame>=b->frames)return;
    int s=b->symbols[frame];if(s==18)return;
    for(int n=0;n<TM_FRAME;n++) {
        double t=n/48000., y=0;
        if(s>=16) y=0.22*sin(2*PI*1000*t)*(s==17?1.-n/960.:1.);
        else {
            for(int h=2;h<=12;h++) {
                double hz=(100+10*s)*h;
                double envelope=exp(-pow((hz-700)/450.,2))+0.7*exp(-pow((hz-1400)/600.,2));
                y+=envelope*sin(2*PI*hz*t)/14.;
            }
            y*=0.65;
        }
        pcm[n]=(int16_t)(y*32767);
    }
}
static int unpack(const uint8_t *symbols,int len,tm_record *r) {
    int steps=len*8+6,n=steps*2;uint8_t code[300],path[150][64],raw[18]={0};
    int cost[64],next[64];
    for(int i=0;i<n/4;i++)for(int j=0;j<4;j++)code[j*(n/4)+i]=(symbols[i]>>(3-j))&1;
    for(int s=0;s<64;s++)cost[s]=s?10000:0;
    for(int t=0;t<steps;t++) {
        for(int s=0;s<64;s++)next[s]=10000;
        for(unsigned s=0;s<64;s++)for(unsigned bit=0;bit<2;bit++) {
            unsigned reg=(s<<1)|bit,dst=reg&63;
            int c=cost[s]+(parity(reg&0171)!=code[2*t])+(parity(reg&0133)!=code[2*t+1]);
            if(c<next[dst]) {next[dst]=c;path[t][dst]=(uint8_t)s;}
        }
        memcpy(cost,next,sizeof(cost));
    }
    unsigned state=0;
    for(int t=steps-1;t>=0;t--) {if(t<len*8)raw[t/8]|=(state&1)<<(7-t%8);state=path[t][state];}
    if(raw[0]!=(len==18?0x11:0x10)||crc16(raw,len)!=0) return 0;
    unsigned id=(unsigned)raw[1]<<16|(unsigned)raw[2]<<8|raw[3];if(id>=11881376)return 0;
    memset(r,0,sizeof(*r));for(int i=4;i>=0;i--) {r->id[i]='A'+id%26;id/=26;}
    r->sequence=(uint16_t)raw[4]<<8|raw[5];r->gps=len==18;
    if(r->gps) {
        uint32_t a=get32(raw+6),o=get32(raw+10);
        r->latitude=(int32_t)(a<=INT32_MAX?(int64_t)a:(int64_t)a-4294967296LL);
        r->longitude=(int32_t)(o<=INT32_MAX?(int64_t)o:(int64_t)o-4294967296LL);
        r->age=(uint16_t)raw[14]<<8|raw[15];
        if(r->latitude < -9000000 || r->latitude > 9000000 || r->longitude < -18000000 || r->longitude > 18000000)return 0;
    }
    return 1;
}
static unsigned classify(const float *samples) {
    float energy=0;for(int n=0;n<320;n++)energy+=samples[n]*samples[n];
    if(energy<320.f*16.f) return 255;
    float best=-1;unsigned symbol=0;
    for(unsigned s=0;s<16;s++) {
        float score=0;
        for(int h=0;h<11;h++) {
            float re=0,im=0;
            for(int n=0;n<320;n++) {re+=samples[n]*basis[s][h][0][n];im+=samples[n]*basis[s][h][1][n];}
            score+=re*re+im*im;
        }
        if(score>best) {best=score;symbol=s;}
    }
    return symbol;
}
unsigned tm_probe_symbol(const int16_t pcm[TM_FRAME]) {
    pthread_once(&once,init_basis);
    float samples[320];
    for(int i=0;i<320;i++) samples[i]=(pcm[3*i]+pcm[3*i+1]+pcm[3*i+2])/3.f;
    return classify(samples);
}
void tm_decoder_reset(tm_decoder *d) {memset(d,0,sizeof(*d));}
int tm_receive(tm_decoder *d,const int16_t *pcm,size_t count,tm_record *record) {
    pthread_once(&once,init_basis);int emitted=0;
    for(size_t i=0;i<count;i++) {
        d->sum+=pcm[i];if(++d->decimation<3)continue;
        float x=d->sum/3.f;d->sum=d->decimation=0;d->samples_seen++;
        if(d->used<320) d->samples[d->used++]=x;
        else {memmove(d->samples,d->samples+1,319*sizeof(float));d->samples[319]=x;}
        if(d->used<320 || ++d->step<80)continue;
        d->step=0;tm_lane *l=&d->lanes[d->lane++%TM_LANES];
        unsigned s=classify(d->samples);
        if(l->collecting) {
            l->symbols[l->count++]=(uint8_t)s;
            tm_record candidate;
            if((l->count==35 && unpack(l->symbols,8,&candidate)) || (l->count==75 && unpack(l->symbols,18,&candidate))) {
                if(!d->have_last || d->samples_seen-d->last_at>48000 || memcmp(&candidate,&d->last,sizeof(candidate))) {
                    *record=candidate;emitted=1;d->last=candidate;d->last_at=d->samples_seen;d->have_last=1;
                }
                l->collecting=0;
            }
            if(l->count==75)l->collecting=0;
        }
        memmove(l->history,l->history+1,15);l->history[15]=(uint8_t)s;
        if(l->seen<16)l->seen++;
        if(l->seen==16) {
            int errors=0;for(int k=0;k<16;k++)errors+=l->history[k]!=sync_word[k];
            if(errors<=1) {l->collecting=1;l->count=0;}
        }
    }
    return emitted;
}
