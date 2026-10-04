/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ptt_telemetry.h"
#include <opus/opus.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void roundtrip(int gps,int codec,int offset,int variant) {
    tm_record input={.id="ALPHA",.sequence=427,.gps=gps,.latitude=-3456789,.longitude=12345678,.age=3},output={0};
    if(variant) {
        unsigned seed=(unsigned)variant*7919;
        for(int i=0;i<5;i++){seed=seed*1664525+1013904223;input.id[i]='A'+seed%26;}
        input.sequence=(uint16_t)seed;
        input.latitude=variant&1?9000000:-9000000;
        input.longitude=variant&1?18000000:-18000000;
    }
    tm_burst b;assert(tm_build(&b,&input));
    tm_decoder *rx=calloc(1,sizeof(*rx));assert(rx);
    int err;OpusEncoder *enc=opus_encoder_create(48000,1,OPUS_APPLICATION_VOIP,&err);assert(enc);
    OpusDecoder *dec=opus_decoder_create(48000,1,&err);assert(dec);
    opus_encoder_ctl(enc,OPUS_SET_BITRATE(24000));opus_encoder_ctl(enc,OPUS_SET_INBAND_FEC(1));opus_encoder_ctl(enc,OPUS_SET_PACKET_LOSS_PERC(10));
    int16_t pcm[960]={0},decoded[960];unsigned char packet[1275];int got=0;
    for(int i=0;i<offset;i++)got+=tm_receive(rx,pcm,1,&output);
    for(unsigned f=0;f<b.frames+5;f++) {
        tm_frame(&b,f,pcm);
        if(codec) {int n=opus_encode(enc,pcm,960,packet,sizeof(packet));assert(n>0);assert(opus_decode(dec,packet,n,decoded,960,0)==960);}
        else memcpy(decoded,pcm,sizeof(pcm));
        /* Erase one decoded payload frame, leaving acquisition intact. The saved
         * experiment separately exercises actual packet loss with Opus PLC. */
        if(variant==1 && f==40) memset(decoded,0,sizeof(decoded));
        if(variant==2) for(int k=0;k<960;k++) decoded[k]=(int16_t)(decoded[k]/3 + ((k*17+(int)f*13)%101)-50);
        got+=tm_receive(rx,decoded,960,&output);
    }
    if(got!=1) {printf("gps=%d opus=%d offset=%d variant=%d events=%d id=%s\n",gps,codec,offset,variant,got,output.id);fflush(stdout);}
    assert(got==1);assert(!memcmp(&input,&output,6));assert(output.sequence==input.sequence&&output.gps==gps);
    if(gps)assert(output.latitude==input.latitude&&output.longitude==input.longitude&&output.age==input.age);
    opus_encoder_destroy(enc);opus_decoder_destroy(dec);free(rx);
}
int main(void) {
    char id[6];assert(tm_id(id,"alpha")&&!strcmp(id,"ALPHA"));assert(!tm_id(id,"AB12C"));assert(!tm_id(id,"ABCD"));
    for(int gps=0;gps<2;gps++)for(int codec=0;codec<2;codec++)for(int off=0;off<3;off++)roundtrip(gps,codec,off*113,0);
    for(int v=1;v<=32;v++) roundtrip(v&1,1,v*23%960,v);
    tm_decoder noise={0};tm_record out;int16_t pcm[960];unsigned rng=12345;
    for(int frame=0;frame<250;frame++) {
        for(int i=0;i<960;i++){rng=rng*1664525+1013904223;pcm[i]=(int16_t)(rng>>16);}
        assert(!tm_receive(&noise,pcm,960,&out));
    }
    tm_burst truncated;tm_record r={.id="ZZZZZ",.gps=1,.latitude=9000001};
    assert(!tm_build(&truncated,&r));r.latitude=0;assert(tm_build(&truncated,&r));
    tm_decoder_reset(&noise);
    for(unsigned frame=0;frame<60;frame++) {tm_frame(&truncated,frame,pcm);assert(!tm_receive(&noise,pcm,960,&out));}
    memset(pcm,0,sizeof(pcm));for(int i=0;i<100;i++)assert(!tm_receive(&noise,pcm,960,&out));
    puts("telemetry: 44 PCM/Opus roundtrips, offsets, attenuation, noise, payload loss, bounds and truncation passed");
}
