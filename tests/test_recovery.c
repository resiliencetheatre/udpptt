/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ptt_jitter.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t packets[64][PTT_OPUS_MAX];
static int lengths[64];
static ptt_jitter_t j;
static packet_hdr_t header(unsigned session, uint32_t seq, int type) {
    packet_hdr_t h; ptt_header_init(&h, type);
    h.session[0] = (uint8_t)session;
    memcpy(h.talk_id, "test", 5);
    h.seq = htonl(seq); h.timestamp = htonl(seq * (uint32_t)PTT_SAMPLES);
    h.samples = htons(type == PKT_AUDIO ? PTT_SAMPLES : 0);
    return h;
}
static int put(unsigned session, uint32_t seq, int64_t now) {
    packet_hdr_t h = header(session, seq, PKT_AUDIO);
    return ptt_jitter_put(&j, &h, packets[seq % 64], lengths[seq % 64], now);
}
static void end(unsigned session, uint32_t seq, int64_t now) {
    packet_hdr_t h = header(session, seq, PKT_END);
    assert(ptt_jitter_put(&j, &h, NULL, 0, now));
}
static void reset(int delay) { ptt_jitter_destroy(&j); ptt_jitter_init(&j, delay, 1); }
int main(void) {
    int err; OpusEncoder *enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
    assert(enc && err == OPUS_OK);
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(20));
    for (int n = 0; n < 64; ++n) {
        int16_t pcm[PTT_SAMPLES];
        for (int k = 0; k < PTT_SAMPLES; ++k)
            pcm[k] = (int16_t)(9000 * sin((n * PTT_SAMPLES + k) * 0.03));
        lengths[n] = opus_encode(enc, pcm, PTT_SAMPLES, packets[n], PTT_OPUS_MAX);
        assert(lengths[n] > 0);
    }
    opus_encoder_destroy(enc);
    int16_t pcm[PTT_SAMPLES], expected[PTT_SAMPLES];

    /* Constant 300 ms transit delay changes startup only, not packet cadence. */
    reset(120);
    for (int t = 0; t <= 620; t += 20) {
        if (t >= 300 && t < 500) assert(put(1, (t - 300) / 20, t));
        if (t == 500) end(1, 10, t);
        unsigned n = ptt_jitter_render(&j, t, pcm);
        assert(n == (unsigned)(t >= 420 && t < 620));
    }
    assert(j.stats.decoded == 10 && j.stats.missing == 0);

    /* Arrival reordering, duplicates, delayed initial frame, END before tail. */
    reset(120);
    assert(put(2, 1, 0)); assert(put(2, 0, 10));
    assert(!put(2, 1, 15)); end(2, 4, 30);
    assert(put(2, 3, 40)); assert(put(2, 2, 90));
    for (int t = 120; t < 200; t += 20) assert(ptt_jitter_render(&j, t, pcm) == 1);
    assert(ptt_jitter_render(&j, 200, pcm) == 0);
    assert(j.stats.decoded == 4 && j.stats.missing == 0 && j.stats.duplicates == 1);
    assert(j.stats.reordered == 2 && !put(2, 2, 210));

    /* A missing frame uses the next packet's FEC path without consuming it. */
    reset(60); assert(put(3, 0, 0)); assert(put(3, 2, 40)); end(3, 3, 50);
    OpusDecoder *reference = opus_decoder_create(48000, 1, &err); assert(reference);
    assert(opus_decode(reference, packets[0], lengths[0], expected, PTT_SAMPLES, 0) == PTT_SAMPLES);
    assert(ptt_jitter_render(&j, 60, pcm) == 1); assert(!memcmp(pcm, expected, sizeof(pcm)));
    assert(opus_decode(reference, packets[2], lengths[2], expected, PTT_SAMPLES, 1) == PTT_SAMPLES);
    assert(ptt_jitter_render(&j, 80, pcm) == 1); assert(!memcmp(pcm, expected, sizeof(pcm)));
    assert(ptt_jitter_render(&j, 100, pcm) == 1);
    assert(j.stats.fec_attempts == 1 && j.stats.missing == 1 && j.stats.decoded == 2);
    opus_decoder_destroy(reference);

    /* Burst loss needs PLC, and late packets never rewind playout. */
    reset(0); assert(put(4, 0, 0)); assert(ptt_jitter_render(&j, 0, pcm) == 1);
    assert(ptt_jitter_render(&j, 20, pcm) == 1);
    assert(ptt_jitter_render(&j, 40, pcm) == 1);
    assert(j.stats.plc == 2 && !put(4, 1, 50));
    for (int t = 60; t <= 300; t += 20) ptt_jitter_render(&j, t, pcm);
    assert(j.stats.rebuffered == 1);
    assert(put(4, 20, 400)); assert(ptt_jitter_render(&j, 400, pcm) == 1);
    /* Timeout permits resuming a still-held PTT session. */
    ptt_jitter_render(&j, 2500, pcm);
    assert(put(4, 130, 2600)); assert(ptt_jitter_render(&j, 2600, pcm) == 1);

    /* Preserve an already-arrived burst beyond a >300 ms loss gap. */
    reset(40); assert(put(4, 0, 0)); assert(put(4, 20, 20)); end(4, 21, 30);
    for (int t = 40; t <= 340; t += 20) ptt_jitter_render(&j, t, pcm);
    assert(j.stats.rebuffered == 1);
    assert(ptt_jitter_render(&j, 380, pcm) == 1 && j.stats.decoded == 2);

    /* Separate decoders, bounded mixing, and local PTT retirement. */
    reset(0); assert(put(5, 0, 0)); assert(put(6, 0, 0));
    assert(ptt_jitter_render(&j, 0, pcm) == 2);
    ptt_jitter_suppress(&j); assert(!put(5, 1, 30));
    assert(ptt_jitter_render(&j, 40, pcm) == 0);
    for (int k = 0; k < PTT_SAMPLES; ++k) assert(pcm[k] == 0);
    /* Completed sessions do not exhaust the eight concurrent stream slots. */
    for (unsigned s = 10; s < 40; ++s) {
        int64_t t = s * 40;
        assert(put(s, 0, t)); end(s, 1, t);
        assert(ptt_jitter_render(&j, t, pcm) == 1);
        assert(ptt_jitter_render(&j, t + 20, pcm) == 0);
    }
    assert(!put(10, 0, 1700));

    /* Capacity remains bounded and an END-only empty session is retired. */
    reset(100);
    for (unsigned s = 1; s <= PTT_STREAMS; ++s) assert(put(s, 0, 0));
    assert(!put(99, 0, 0) && j.stats.overflow == 1);
    reset(100);
    for (unsigned s = 1; s <= 20; ++s) end(s, 0, s);
    assert(put(99, 0, 30));

    /* A jump beyond the ring capacity refills without unbounded delay. */
    reset(40);
    assert(put(1, 0, 0)); assert(ptt_jitter_render(&j, 40, pcm) == 1);
    assert(put(1, 200, 60));
    assert(j.stats.rebuffered == 1 && ptt_jitter_render(&j, 60, pcm) == 0);
    assert(ptt_jitter_render(&j, 100, pcm) == 1);

    /* Ring wrap and sequence/timestamp wrap. */
    reset(0);
    assert(put(40, UINT32_MAX, 0)); assert(ptt_jitter_render(&j, 0, pcm) == 1);
    assert(put(40, 0, 20)); assert(ptt_jitter_render(&j, 20, pcm) == 1);
    assert(j.stats.missing == 0);
    packet_hdr_t h = header(1, UINT32_MAX, PKT_AUDIO);
    h.len = htons(3); assert(ptt_header_valid(&h, sizeof(h) + 3));
    h.timestamp ^= 1; assert(!ptt_header_valid(&h, sizeof(h) + 3));
    h = header(1, 0, PKT_AUDIO); h.len = htons(PTT_OPUS_MAX + 1);
    assert(!ptt_header_valid(&h, MAX_PACKET));
    h = header(1, 0, PKT_END); assert(ptt_header_valid(&h, sizeof(h)));
    h.version = 1; assert(!ptt_header_valid(&h, sizeof(h)));
    reset(100);
    h = header(1, 0, PKT_AUDIO);
    uint8_t invalid[] = {0xff};
    assert(!ptt_jitter_put(&j, &h, invalid, sizeof(invalid), 0));
    /* Decode each telemetry stream before mixing simultaneous talkers. */
    reset(0);
    tm_record records[2] = {{.id="ALPHA", .sequence=9}, {.id="BRAVO", .sequence=10, .gps=1, .latitude=4961160, .longitude=613190}};
    tm_burst bursts[2]; OpusEncoder *encoders[2];
    for (int i=0;i<2;i++) {
        assert(tm_build(&bursts[i], &records[i]));
        encoders[i]=opus_encoder_create(48000,1,OPUS_APPLICATION_VOIP,&err); assert(encoders[i]);
        opus_encoder_ctl(encoders[i], OPUS_SET_BITRATE(24000));
    }
    for (unsigned frame=0;frame<TM_MAX_FRAMES+3;frame++) {
        for (int i=0;i<2;i++) {
            tm_frame(&bursts[i], frame, pcm);
            uint8_t packet[PTT_OPUS_MAX];
            int bytes=opus_encode(encoders[i],pcm,PTT_SAMPLES,packet,sizeof(packet)); assert(bytes>0);
            h=header((unsigned)i+1,frame,PKT_AUDIO);
            assert(ptt_jitter_put(&j,&h,packet,(size_t)bytes,frame*20));
        }
        assert(ptt_jitter_render(&j,frame*20,pcm)==2);
    }
    assert(j.event_write==2 && j.event_drops==0);
    assert(!strcmp(j.events[0].id,"ALPHA") && !strcmp(j.events[1].id,"BRAVO"));
    assert(j.events[1].gps && j.events[1].latitude==4961160);
    for (int i=0;i<2;i++) opus_encoder_destroy(encoders[i]);
    ptt_jitter_destroy(&j);
    puts("recovery tests passed");
    return 0;
}
