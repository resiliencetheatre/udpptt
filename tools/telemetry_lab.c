/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Small reproducible 48 kHz mono PCM WAV laboratory and audio-only receiver. */
#include "ptt_telemetry.h"
#include <opus/opus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
static void put16(FILE *f,unsigned n) { fputc(n&255,f);fputc((n>>8)&255,f); }
static void put32(FILE *f,unsigned n) {put16(f,n);put16(f,n>>16);}
static unsigned get16(FILE *f) {int a=fgetc(f),b=fgetc(f);return a<0||b<0?UINT_MAX:(unsigned)a|((unsigned)b<<8);}
static unsigned get32(FILE *f) {unsigned a=get16(f),b=get16(f);return a|b<<16;}
static FILE *wav_create(const char *path,unsigned samples) {
    FILE *f=fopen(path,"wbx");if(!f)return NULL;
    fwrite("RIFF",1,4,f);put32(f,36+samples*2);fwrite("WAVEfmt ",1,8,f);put32(f,16);
    put16(f,1);put16(f,1);put32(f,48000);put32(f,96000);put16(f,2);put16(f,16);
    fwrite("data",1,4,f);put32(f,samples*2);return f;
}
static FILE *wav_open(const char *path,unsigned *samples) {
    FILE *f=fopen(path,"rb");if(!f)return NULL;char tag[4];int format=0;
    if(fread(tag,1,4,f)!=4||memcmp(tag,"RIFF",4))goto bad;
    (void)get32(f);if(fread(tag,1,4,f)!=4||memcmp(tag,"WAVE",4))goto bad;
    while(fread(tag,1,4,f)==4) {
        unsigned size=get32(f);if(size>INT_MAX)goto bad;
        if(!memcmp(tag,"fmt ",4)) {
            if(size<16)goto bad;
            unsigned pcm=get16(f),channels=get16(f),rate=get32(f),bytes=get32(f),align=get16(f),bits=get16(f);
            format=pcm==1&&channels==1&&rate==48000&&bytes==96000&&align==2&&bits==16;
            if(fseek(f,(long)(size-16+(size&1)),SEEK_CUR))goto bad;
        } else if(!memcmp(tag,"data",4)) {if(!format||size%2)goto bad;*samples=size/2;return f;}
        else if(fseek(f,(long)(size+(size&1)),SEEK_CUR))goto bad;
    }
bad:fclose(f);return NULL;
}
static int read_pcm(FILE *f,int16_t *pcm,unsigned n) {
    for(unsigned i=0;i<n;i++) {unsigned v=get16(f);if(v==UINT_MAX)return 0;pcm[i]=(int16_t)(v<=32767?(int)v:(int)v-65536);}return 1;
}
static void write_pcm(FILE *f,const int16_t *pcm,unsigned n) {for(unsigned i=0;i<n;i++)put16(f,(uint16_t)pcm[i]);}
static int number(const char *s,double *v) {char *end;*v=strtod(s,&end);return *s&&!*end&&isfinite(*v);}
static int confusion(const char *prefix) {
    enum {FRAMES=517, SYMBOLS=512};
    int16_t *tx=calloc(FRAMES*960,sizeof(*tx)),*rx=calloc(FRAMES*960,sizeof(*rx));
    if(!tx||!rx)return 1;
    unsigned expected[SYMBOLS],matrix[2][16][17]={0},errors[2]={0};
    tm_burst b={.frames=1};
    for(unsigned i=0;i<SYMBOLS;i++) {
        expected[i]=i%2?(i/2)%16:(i/2)/16;
        b.symbols[0]=(uint8_t)expected[i];tm_frame(&b,0,tx+i*960);
    }
    int err,lookahead;OpusEncoder *enc=opus_encoder_create(48000,1,OPUS_APPLICATION_VOIP,&err);
    OpusDecoder *dec=opus_decoder_create(48000,1,&err);if(!enc||!dec)return 1;
    opus_encoder_ctl(enc,OPUS_SET_BITRATE(24000));opus_encoder_ctl(enc,OPUS_SET_INBAND_FEC(1));opus_encoder_ctl(enc,OPUS_SET_PACKET_LOSS_PERC(10));
    opus_encoder_ctl(enc,OPUS_GET_LOOKAHEAD(&lookahead));
    if(lookahead<0||lookahead>5*960)return 1;
    for(int i=0;i<FRAMES;i++) {
        unsigned char packet[1275];int n=opus_encode(enc,tx+i*960,960,packet,sizeof(packet));
        if(n<0||opus_decode(dec,packet,n,rx+i*960,960,0)!=960)return 1;
    }
    opus_encoder_destroy(enc);opus_decoder_destroy(dec);
    for(unsigned i=0;i<SYMBOLS;i++)for(int channel=0;channel<2;channel++) {
        unsigned got=tm_probe_symbol(channel?rx+i*960+lookahead:tx+i*960);
        matrix[channel][expected[i]][got<16?got:16]++;errors[channel]+=got!=expected[i];
    }
    for(int channel=0;channel<2;channel++) {
        char path[4096];
        if(snprintf(path,sizeof(path),"%s-%s.wav",prefix,channel?"opus":"raw") >= (int)sizeof(path))return 1;
        FILE *f=wav_create(path,FRAMES*960);if(!f)return 1;write_pcm(f,channel?rx:tx,FRAMES*960);
        int bad=ferror(f);bad|=fclose(f)!=0;if(bad)return 1;
        if(snprintf(path,sizeof(path),"%s-%s.csv",prefix,channel?"opus":"raw") >= (int)sizeof(path))return 1;
        f=fopen(path,"wx");if(!f)return 1;
        fprintf(f,"expected");for(int col=0;col<17;col++)fprintf(f,",%d",col);fputc('\n',f);
        for(int row=0;row<16;row++){fprintf(f,"%d",row);for(int col=0;col<17;col++)fprintf(f,",%u",matrix[channel][row][col]);fputc('\n',f);}
        bad=ferror(f);bad|=fclose(f)!=0;if(bad)return 1;
    }
    free(tx);free(rx);
    printf("{\"symbols\":512,\"raw_errors\":%u,\"opus_errors\":%u,\"opus_lookahead_samples\":%d}\n",errors[0],errors[1],lookahead);
    return 0;
}
int main(int argc,char **argv) {
    if(argc==3&&!strcmp(argv[1],"confusion"))return confusion(argv[2]);
    if(argc>=4&&!strcmp(argv[1],"encode")) {
        tm_record r={.sequence=1};tm_burst burst;
        if(!tm_id(r.id,argv[2])||(argc!=4&&argc!=6))return 2;
        if(argc==6) {double lat,lon;if(!number(argv[4],&lat)||!number(argv[5],&lon)||lat < -90||lat>90||lon < -180||lon>180)return 2;r.gps=1;r.latitude=(int32_t)llround(lat*100000);r.longitude=(int32_t)llround(lon*100000);}
        if(!tm_build(&burst,&r))return 2;
        FILE *f=wav_create(argv[3],burst.frames*960);if(!f){perror(argv[3]);return 1;}
        for(unsigned i=0;i<burst.frames;i++){int16_t pcm[960];tm_frame(&burst,i,pcm);write_pcm(f,pcm,960);}
        int bad=ferror(f);bad|=fclose(f)!=0;
        printf("{\"frames\":%u,\"duration_ms\":%u,\"sample_rate\":48000}\n",burst.frames,burst.frames*20);return bad?1:0;
    }
    if(argc==3&&!strcmp(argv[1],"decode")) {
        unsigned samples;FILE *f=wav_open(argv[2],&samples);if(!f){fprintf(stderr,"expected 48 kHz mono PCM16 WAV\n");return 1;}
        tm_decoder decoder={0};tm_record r;unsigned events=0;
        for(unsigned pos=0;pos<samples;) {
            int16_t pcm[960];unsigned n=samples-pos<960?samples-pos:960;
            if(!read_pcm(f,pcm,n)){fclose(f);return 1;}pos+=n;
            if(tm_receive(&decoder,pcm,n,&r)) {
                events++;printf("{\"id\":\"%s\",\"sequence\":%u,\"gps\":%s",r.id,r.sequence,r.gps?"true":"false");
                if(r.gps)printf(",\"latitude\":%.5f,\"longitude\":%.5f,\"fix_age_s\":%u",r.latitude/100000.,r.longitude/100000.,r.age);
                printf(",\"sample\":%u}\n",pos);
            }
        }
        fclose(f);return events?0:3;
    }
    if(argc==6&&!strcmp(argv[1],"opus")) {
        double loss,seed;if(!number(argv[4],&loss)||loss<0||loss>100||!number(argv[5],&seed)||seed<1||seed>UINT_MAX||floor(seed)!=seed)return 2;
        unsigned samples;FILE *in=wav_open(argv[2],&samples);if(!in)return 1;
        unsigned frames=(samples+959)/960;
        FILE *out=wav_create(argv[3],frames*960);if(!out){fclose(in);return 1;}
        char packet_path[4096];
        if(snprintf(packet_path,sizeof(packet_path),"%s.packets",argv[3]) >= (int)sizeof(packet_path))return 1;
        FILE *packets=fopen(packet_path,"wbx");if(!packets)return 1;
        int err;OpusEncoder *enc=opus_encoder_create(48000,1,OPUS_APPLICATION_VOIP,&err);OpusDecoder *dec=opus_decoder_create(48000,1,&err);
        if(!enc||!dec)return 1;
        opus_encoder_ctl(enc,OPUS_SET_BITRATE(24000));opus_encoder_ctl(enc,OPUS_SET_INBAND_FEC(1));opus_encoder_ctl(enc,OPUS_SET_PACKET_LOSS_PERC(10));
        uint32_t rng=(uint32_t)seed;unsigned lost=0;
        for(unsigned i=0;i<frames;i++) {
            int16_t pcm[960]={0},decoded[960];unsigned char packet[1275];unsigned n=samples<960?samples:960;
            if(!read_pcm(in,pcm,n))return 1;
            samples-=n;
            int bytes=opus_encode(enc,pcm,960,packet,sizeof(packet));if(bytes<0)return 1;
            rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;int drop=rng/(double)UINT_MAX < loss/100.;lost+=drop;
            put16(packets,(unsigned)bytes);fputc(drop,packets);fwrite(packet,1,(size_t)bytes,packets);
            if(opus_decode(dec,drop?NULL:packet,drop?0:bytes,decoded,960,0)!=960)return 1;
            write_pcm(out,decoded,960);
        }
        opus_encoder_destroy(enc);opus_decoder_destroy(dec);fclose(in);int bad=ferror(out)|ferror(packets);bad|=fclose(packets)!=0;bad|=fclose(out)!=0;
        printf("{\"codec\":\"opus\",\"bitrate\":24000,\"frames\":%u,\"lost\":%u,\"seed\":%.0f,\"loss_percent\":%.3f}\n",frames,lost,seed,loss);return bad?1:0;
    }
    fprintf(stderr,"usage (also: telemetry_lab confusion output-prefix):\n  %s encode ABCDE out.wav [latitude longitude]\n  %s decode in.wav\n  %s opus in.wav out.wav loss_percent seed\n",argv[0],argv[0],argv[0]);return 2;
}
