/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PTT_PROTOCOL_H
#define PTT_PROTOCOL_H

#include <arpa/inet.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define MAX_PACKET 2048
#define TALK_ID_MAX 31
#define NONCE_LEN 24
#define PKT_IDLE 0
#define PKT_AUDIO 1
#define PKT_END 2
#define PKT_FLAG_ENCRYPTED 1
#define PTT_VERSION 2
#define PTT_SAMPLES 960
#define PTT_FRAME_MS 20
#define PTT_OPUS_MAX 1275
#define PTT_TAG_BYTES 16

/* All integers wider than one byte are network order. Sessions are random
 * opaque bytes, renewed for every PTT press. END.seq is the exclusive end. */
typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint8_t version, type, flags, reserved;
    uint16_t len, samples;
    uint32_t seq, timestamp;
    uint8_t session[16];
    char talk_id[TALK_ID_MAX + 1];
    uint8_t nonce[NONCE_LEN];
} packet_hdr_t;

_Static_assert(sizeof(packet_hdr_t) == 92, "protocol layout changed");

static inline void ptt_header_init(packet_hdr_t *h, uint8_t type) {
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, "UPTT", 4);
    h->version = PTT_VERSION;
    h->type = type;
}

/* Ethernet may pad small frames, so trailing bytes are allowed. */
static inline int ptt_header_valid(const packet_hdr_t *h, size_t bytes) {
    if (bytes < sizeof(*h) || memcmp(h->magic, "UPTT", 4) ||
        h->version != PTT_VERSION || h->reserved ||
        (h->flags & ~PKT_FLAG_ENCRYPTED) ||
        !memchr(h->talk_id, 0, sizeof(h->talk_id))) return 0;
    unsigned len = ntohs(h->len);
    unsigned tag = (h->flags & PKT_FLAG_ENCRYPTED) ? PTT_TAG_BYTES : 0;
    if (len < tag || sizeof(*h) + len > bytes) return 0;
    if (h->type == PKT_IDLE)
        return len == tag && h->samples == 0 && h->seq == 0 && h->timestamp == 0;
    if (h->type != PKT_AUDIO && h->type != PKT_END) return 0;
    static const uint8_t zero[16] = {0};
    if (!memcmp(h->session, zero, 16) ||
        ntohl(h->timestamp) != ntohl(h->seq) * (uint32_t)PTT_SAMPLES) return 0;
    if (h->type == PKT_END) return len == tag && h->samples == 0;
    return ntohs(h->samples) == PTT_SAMPLES && len > tag && len <= PTT_OPUS_MAX + tag;
}

static inline int32_t ptt_seq_diff(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}
#endif
