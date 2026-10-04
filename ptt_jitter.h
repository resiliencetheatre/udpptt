/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PTT_JITTER_H
#define PTT_JITTER_H
#include "ptt_protocol.h"
#include "ptt_telemetry.h"
#include <opus/opus.h>
#include <stdint.h>

#define PTT_STREAMS 8
#define PTT_SLOTS 128

typedef struct {
    uint32_t seq;
    uint16_t len;
    uint8_t data[PTT_OPUS_MAX];
} ptt_slot_t;

typedef struct {
    int used, active, started, have_audio, have_end, rebuffer;
    uint8_t session[16];
    char talk_id[TALK_ID_MAX + 1];
    uint32_t next, highest, end;
    int64_t due_ms, last_ms, last_audio_ms;
    unsigned missing_run;
    OpusDecoder *decoder;
    tm_decoder telemetry;
    ptt_slot_t slots[PTT_SLOTS];
} ptt_stream_t;

typedef struct {
    uint64_t accepted, decoded, missing, fec_attempts, plc, late, duplicates;
    uint64_t reordered, invalid, overflow, rebuffered, timeouts, suppressed;
    double jitter_ms;
} ptt_rx_stats_t;

/* Caller serializes access. Each render produces one 20 ms mono PCM block.
 * Independent sessions have independent Opus decoders and are mixed locally. */
typedef struct {
    int delay_ms, fec;
    tm_record events[16];
    unsigned event_read, event_write;
    uint64_t event_drops;
    ptt_stream_t streams[PTT_STREAMS];
    struct { uint8_t session[16]; int64_t until_ms; } retired[64];
    unsigned retired_next;
    ptt_rx_stats_t stats;
} ptt_jitter_t;

void ptt_jitter_init(ptt_jitter_t *j, int delay_ms, int fec);
void ptt_jitter_destroy(ptt_jitter_t *j);
void ptt_jitter_suppress(ptt_jitter_t *j);
int ptt_jitter_put(ptt_jitter_t *j, const packet_hdr_t *h,
                   const uint8_t *data, size_t len, int64_t now);
unsigned ptt_jitter_render(ptt_jitter_t *j, int64_t now, int16_t pcm[PTT_SAMPLES]);
#endif
