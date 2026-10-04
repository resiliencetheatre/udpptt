/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PTT_TELEMETRY_H
#define PTT_TELEMETRY_H
#include <stddef.h>
#include <stdint.h>
#define TM_FRAME 960
#define TM_MAX_FRAMES 111
#define TM_LANES 4
/* Experimental v1: 16 voiced pitches, 20 ms symbols, rate 1/2 K=7 FEC. */
typedef struct {
    char id[6];
    uint16_t sequence, age;
    int gps;
    int32_t latitude, longitude; /* degrees * 100000 */
} tm_record;
typedef struct {
    uint8_t symbols[TM_MAX_FRAMES];
    unsigned frames;
} tm_burst;
typedef struct {
    uint8_t history[16], symbols[75];
    unsigned seen, count;
    int collecting;
} tm_lane;
typedef struct {
    float samples[320];
    unsigned used, step, lane;
    int sum, decimation;
    tm_lane lanes[TM_LANES];
    tm_record last;
    uint64_t samples_seen, last_at;
    int have_last;
} tm_decoder;
int tm_id(char out[6], const char *in);
int tm_build(tm_burst *burst, const tm_record *record);
void tm_frame(const tm_burst *burst, unsigned frame, int16_t pcm[TM_FRAME]);
/* Offline, aligned 20 ms symbol diagnostic; 255 means silence. */
unsigned tm_probe_symbol(const int16_t pcm[TM_FRAME]);
void tm_decoder_reset(tm_decoder *decoder);
/* Streaming 48 kHz mono input; emits at most one event per call <= 960 samples. */
int tm_receive(tm_decoder *decoder, const int16_t *pcm, size_t count, tm_record *record);
#endif
