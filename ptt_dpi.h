/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PTT_DPI_H
#define PTT_DPI_H
#include "dpi/mdpx.h"
#include "ptt_protocol.h"
/* Structural decoding only. No signature or identity verification here.
 * Ethernet can add padding, but UDP must have exactly the declared length. */
static inline int ptt_dpi_unwrap(const uint8_t **data, size_t *length, int ethernet) {
    if (*length < 4 || memcmp(*data, "MDPX", 4)) return 0;
    size_t size = *length;
    if (ethernet && size >= 11) {
        size_t declared = (size_t)(*data)[6] * 256 + (*data)[7]
                        + (size_t)(*data)[9] * 256 + (*data)[10];
        if (declared < size) size = declared;
    }
    struct packet p;
    if (decode(*data, size, &p)) return -1;
    if (p.payload_length < sizeof(packet_hdr_t)) return -1;
    packet_hdr_t h;
    memcpy(&h, p.payload, sizeof h);
    if (!ptt_header_valid(&h, p.payload_length) ||
        sizeof h + ntohs(h.len) != p.payload_length) return -1;
    *data = p.payload;
    *length = p.payload_length;
    return 1;
}
#endif
