/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ptt_jitter.h"
#include <limits.h>
#include <stdlib.h>

void ptt_jitter_init(ptt_jitter_t *j, int delay_ms, int fec) {
    memset(j, 0, sizeof(*j));
    j->delay_ms = delay_ms;
    j->fec = fec;
}

void ptt_jitter_destroy(ptt_jitter_t *j) {
    for (int i = 0; i < PTT_STREAMS; ++i) {
        if (j->streams[i].decoder) opus_decoder_destroy(j->streams[i].decoder);
        j->streams[i].decoder = NULL;
    }
}

static void retire(ptt_jitter_t *j, ptt_stream_t *s, int64_t now) {
    unsigned i = j->retired_next++ % 64;
    memcpy(j->retired[i].session, s->session, 16);
    j->retired[i].until_ms = now + 30000;
    s->active = 0;
}

void ptt_jitter_suppress(ptt_jitter_t *j) {
    for (int i = 0; i < PTT_STREAMS; ++i) {
        ptt_stream_t *s = &j->streams[i];
        if (s->active) { retire(j, s, s->last_ms); j->stats.suppressed++; }
    }
}

static ptt_stream_t *stream_for(ptt_jitter_t *j, const packet_hdr_t *h, int64_t now) {
    ptt_stream_t *free_slot = NULL;
    for (unsigned i = 0; i < 64; ++i) {
        if (j->retired[i].until_ms > now && !memcmp(j->retired[i].session, h->session, 16)) {
            j->stats.late++; return NULL;
        }
    }
    for (int i = 0; i < PTT_STREAMS; ++i) {
        ptt_stream_t *s = &j->streams[i];
        if (s->used && !memcmp(s->session, h->session, 16) &&
            !memcmp(s->talk_id, h->talk_id, sizeof(s->talk_id))) {
            if (s->active) return s;
            free_slot = s; break;
        }
        if (!s->used || !s->active) free_slot = s;
    }
    if (!free_slot) { j->stats.overflow++; return NULL; }
    OpusDecoder *decoder = free_slot->decoder;
    if (!decoder) {
        int err;
        decoder = opus_decoder_create(48000, 1, &err);
        if (!decoder) { j->stats.invalid++; return NULL; }
    } else opus_decoder_ctl(decoder, OPUS_RESET_STATE);
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->decoder = decoder;
    free_slot->used = free_slot->active = 1;
    memcpy(free_slot->session, h->session, 16);
    memcpy(free_slot->talk_id, h->talk_id, sizeof(free_slot->talk_id));
    free_slot->last_ms = now;
    return free_slot;
}

int ptt_jitter_put(ptt_jitter_t *j, const packet_hdr_t *h,
                   const uint8_t *data, size_t len, int64_t now) {
    if (h->type != PKT_AUDIO && h->type != PKT_END) return 0;
    if (h->type == PKT_AUDIO && (len == 0 || len > PTT_OPUS_MAX ||
        opus_packet_get_nb_samples(data, (opus_int32)len, 48000) != PTT_SAMPLES)) {
        j->stats.invalid++; return 0;
    }
    ptt_stream_t *s = stream_for(j, h, now);
    if (!s) return 0;
    if (!s->active) { j->stats.late++; return 0; }
    uint32_t seq = ntohl(h->seq);
    if (h->type == PKT_END) {
        if ((s->have_end && s->end != seq) ||
            (s->have_audio && ptt_seq_diff(seq, s->highest) <= 0)) {
            j->stats.invalid++; return 0;
        }
        s->have_end = 1; s->end = seq; s->last_ms = now;
        if (!s->have_audio && seq == 0) retire(j, s, now);
        return 1;
    }
    if (s->have_end && ptt_seq_diff(seq, s->end) >= 0) { j->stats.late++; return 0; }
    if (!s->have_audio || s->rebuffer) {
        s->next = s->highest = seq;
        s->due_ms = now + j->delay_ms;
        s->started = s->rebuffer = 0;
        s->missing_run = 0;
        s->have_audio = 1;
    }
    int32_t delta = ptt_seq_diff(seq, s->next);
    if (delta < 0) {
        if (!s->started && delta > -PTT_SLOTS &&
            ptt_seq_diff(s->highest, seq) < PTT_SLOTS) s->next = seq;
        else { j->stats.late++; return 0; }
    } else if (delta >= PTT_SLOTS) {
        /* Bound both memory and latency after an outage or scheduler stall. */
        memset(s->slots, 0, sizeof(s->slots));
        opus_decoder_ctl(s->decoder, OPUS_RESET_STATE);
        tm_decoder_reset(&s->telemetry);
        s->next = s->highest = seq;
        s->due_ms = now + j->delay_ms;
        s->started = 0;
        j->stats.rebuffered++;
    }
    ptt_slot_t *p = &s->slots[seq % PTT_SLOTS];
    if (p->len && p->seq == seq) { j->stats.duplicates++; return 0; }
    int32_t advance = ptt_seq_diff(seq, s->highest);
    if (advance < 0) j->stats.reordered++;
    if (advance > 0) {
        if (s->last_audio_ms) {
            double d = (double)(now - s->last_audio_ms) - (double)advance * PTT_FRAME_MS;
            if (d < 0) d = -d;
            j->stats.jitter_ms += (d - j->stats.jitter_ms) / 16.0;
        }
        s->highest = seq;
    }
    p->seq = seq; p->len = (uint16_t)len;
    memcpy(p->data, data, len);
    s->last_ms = now;
    if (advance >= 0) s->last_audio_ms = now;
    j->stats.accepted++;
    return 1;
}

unsigned ptt_jitter_render(ptt_jitter_t *j, int64_t now, int16_t pcm[PTT_SAMPLES]) {
    int32_t mixed[PTT_SAMPLES] = {0};
    unsigned rendered = 0;
    for (int i = 0; i < PTT_STREAMS; ++i) {
        ptt_stream_t *s = &j->streams[i];
        if (!s->active) continue;
        if (now - s->last_ms > 2000) {
            if (s->have_end) retire(j, s, now);
            else s->active = 0;
            j->stats.timeouts++; continue;
        }
        if (!s->have_audio || s->rebuffer || now < s->due_ms) continue;
        if (s->have_end && ptt_seq_diff(s->next, s->end) >= 0) {
            retire(j, s, now); s->last_ms = now; continue;
        }
        s->started = 1;
        ptt_slot_t *p = &s->slots[s->next % PTT_SLOTS];
        int16_t decoded[PTT_SAMPLES];
        int n;
        if (p->len && p->seq == s->next) {
            n = opus_decode(s->decoder, p->data, p->len, decoded, PTT_SAMPLES, 0);
            p->len = 0;
            s->missing_run = 0;
            if (n == PTT_SAMPLES) j->stats.decoded++;
        } else {
            j->stats.missing++;
            s->missing_run++;
            ptt_slot_t *next = &s->slots[(s->next + 1) % PTT_SLOTS];
            if (j->fec && next->len && next->seq == s->next + 1) {
                /* libopus falls back to PLC if this packet contains no FEC.
                 * Count attempts, never claim that all attempts recovered audio. */
                n = opus_decode(s->decoder, next->data, next->len, decoded, PTT_SAMPLES, 1);
                j->stats.fec_attempts++;
            } else {
                n = opus_decode(s->decoder, NULL, 0, decoded, PTT_SAMPLES, 0);
                j->stats.plc++;
            }
        }
        if (n != PTT_SAMPLES) {
            j->stats.invalid++;
            n = opus_decode(s->decoder, NULL, 0, decoded, PTT_SAMPLES, 0);
            j->stats.plc++;
            if (n != PTT_SAMPLES) memset(decoded, 0, sizeof(decoded));
        }
        tm_record event;
        if (tm_receive(&s->telemetry, decoded, PTT_SAMPLES, &event)) {
            if (j->event_write - j->event_read < 16)
                j->events[j->event_write++ % 16] = event;
            else j->event_drops++;
        }
        for (int k = 0; k < PTT_SAMPLES; ++k) mixed[k] += decoded[k];
        rendered++;
        s->next++;
        s->due_ms += PTT_FRAME_MS;
        if (s->due_ms < now) s->due_ms = now + PTT_FRAME_MS;
        /* Do not synthesize indefinite speech when END was lost. Refill on
         * fresh audio after 300 ms of consecutive missing frames. */
        if (s->missing_run >= 15) {
            s->rebuffer = 1;
            /* A later burst may already be buffered beyond a long loss gap.
             * Resume at its first frame instead of throwing it away. */
            uint32_t first = 0;
            int found = 0;
            for (unsigned k = 0; k < PTT_SLOTS; ++k) {
                ptt_slot_t *queued = &s->slots[k];
                if (!queued->len) continue;
                if (ptt_seq_diff(queued->seq, s->next) < 0) { queued->len = 0; continue; }
                if (!found || ptt_seq_diff(queued->seq, first) < 0) {
                    first = queued->seq; found = 1;
                }
            }
            if (found) {
                s->next = first; s->due_ms = now + j->delay_ms;
                s->rebuffer = s->started = 0; s->missing_run = 0;
            }
            opus_decoder_ctl(s->decoder, OPUS_RESET_STATE);
        tm_decoder_reset(&s->telemetry);
            j->stats.rebuffered++;
        }
    }
    for (int k = 0; k < PTT_SAMPLES; ++k) {
        int32_t v = mixed[k];
        pcm[k] = v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : (int16_t)v;
    }
    return rendered;
}
