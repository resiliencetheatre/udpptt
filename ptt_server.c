/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "ptt_protocol.h"

#define DEFAULT_SERVER_PORT 5000
#define MAX_CLIENTS 128
#define CLIENT_TIMEOUT_MS 30000
#define CLOSED_SESSIONS 16

typedef struct {
    int used;
    struct sockaddr_in addr;
    int64_t last_seen_ms;
    uint8_t closed[CLOSED_SESSIONS][16];
    unsigned closed_next;
    unsigned long rx_audio, forwarded, rejected;
} client_t;

typedef struct {
    int client, ending;
    uint8_t session[16];
    char talk_id[TALK_ID_MAX + 1];
    uint32_t end_seq;
    int64_t last_audio_ms, release_ms;
} talker_t;

static volatile sig_atomic_t running = 1;
static int64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void on_signal(int sig) { (void)sig; running = 0; }
static int number(const char *s, int lo, int hi) {
    char *end; errno = 0; long v = strtol(s, &end, 10);
    return errno || !*s || *end || v < lo || v > hi ? -1 : (int)v;
}
static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}
static int closed_session(const client_t *c, const uint8_t session[16]) {
    for (int i = 0; i < CLOSED_SESSIONS; ++i)
        if (!memcmp(c->closed[i], session, 16)) return 1;
    return 0;
}
static void release_talker(client_t *clients, talker_t *t, const char *reason) {
    if (t->client < 0) return;
    client_t *c = &clients[t->client];
    /* A silence timeout releases arbitration but must allow the same PTT
     * session to resume after a long mobile outage. */
    if (t->ending || !strcmp(reason, "new session"))
        memcpy(c->closed[c->closed_next++ % CLOSED_SESSIONS], t->session, 16);
    printf("active talker released: id=%s reason=%s\n", t->talk_id, reason);
    fflush(stdout);
    t->client = -1; t->ending = 0;
}
static void maintain(client_t *clients, talker_t *t, int hold, int64_t now) {
    if (t->client >= 0) {
        if (t->ending && now >= t->release_ms) release_talker(clients, t, "PTT end");
        else if (!t->ending && now - t->last_audio_ms >= hold) release_talker(clients, t, "audio timeout");
    }
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].used && now - clients[i].last_seen_ms >= CLIENT_TIMEOUT_MS) {
            if (t->client == i) release_talker(clients, t, "client timeout");
            clients[i].used = 0;
        }
    }
}

int main(int argc, char **argv) {
    int port = DEFAULT_SERVER_PORT, hold = 1000, grace = 100;
    for (int i = 1; i < argc; ++i) {
        int *target, lo, hi;
        if (!strcmp(argv[i], "--port") || !strcmp(argv[i], "--udp-port")) {
            target = &port; lo = 1; hi = 65535;
        } else if (!strcmp(argv[i], "--talker-hold-ms")) {
            target = &hold; lo = 100; hi = 60000;
        } else if (!strcmp(argv[i], "--release-grace-ms")) {
            target = &grace; lo = 0; hi = 2000;
        } else {
            fprintf(stderr, "usage: %s [--port PORT] [--talker-hold-ms 100..60000] [--release-grace-ms 0..2000]\n", argv[0]);
            return 1;
        }
        if (++i >= argc || (*target = number(argv[i], lo, hi)) < 0) {
            fprintf(stderr, "invalid or missing option value\n"); return 1;
        }
    }
    if (grace > hold) { fprintf(stderr, "release grace must not exceed talker hold\n"); return 1; }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) { perror("SO_RCVTIMEO"); close(sock); return 1; }
    struct sockaddr_in srv = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) { perror("bind"); close(sock); return 1; }
    client_t clients[MAX_CLIENTS] = {0};
    talker_t talker = {.client = -1};
    unsigned long invalid = 0, rejected = 0, forwarded = 0, send_errors = 0, audio = 0;
    int64_t next_report = now_ms() + 5000;
    printf("ptt_server UDP port=%d protocol=v2 talker_hold_ms=%d release_grace_ms=%d\n", port, hold, grace);
    fflush(stdout);
    while (running) {
        uint8_t buf[MAX_PACKET]; struct sockaddr_in src; socklen_t src_len = sizeof(src);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &src_len);
        int64_t now = now_ms();
        /* Expire before processing an arriving packet, including after a wait. */
        maintain(clients, &talker, hold, now);
        if (now >= next_report) {
            printf("server stats: audio=%lu forwarded=%lu rejected=%lu invalid=%lu send_errors=%lu\n", audio, forwarded, rejected, invalid, send_errors);
            fflush(stdout); next_report = now + 5000;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            perror("recvfrom"); break;
        }
        packet_hdr_t hdr;
        if ((size_t)n < sizeof(hdr)) { invalid++; continue; }
        memcpy(&hdr, buf, sizeof(hdr));
        if (!ptt_header_valid(&hdr, (size_t)n)) {
            if (++invalid == 1 || invalid % 100 == 0) fprintf(stderr, "invalid packet: protocol v2 required (count=%lu)\n", invalid);
            continue;
        }
        int idx = -1, free_idx = -1;
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i].used && same_addr(&clients[i].addr, &src)) { idx = i; break; }
            if (!clients[i].used) free_idx = i;
        }
        if (idx < 0) {
            if (free_idx < 0) { rejected++; continue; }
            idx = free_idx; memset(&clients[idx], 0, sizeof(clients[idx]));
            clients[idx].used = 1; clients[idx].addr = src;
        }
        client_t *c = &clients[idx]; c->last_seen_ms = now;
        if (hdr.type == PKT_IDLE) continue;
        if (hdr.type == PKT_AUDIO) { c->rx_audio++; audio++; }
        if (closed_session(c, hdr.session)) { c->rejected++; rejected++; continue; }
        if (talker.client == idx && memcmp(talker.session, hdr.session, 16)) {
            /* New PTT press from the current owner supersedes its previous
             * session. Remember the old ID so reordered END cannot release it. */
            if (hdr.type == PKT_AUDIO) release_talker(clients, &talker, "new session");
            else { c->rejected++; rejected++; continue; }
        }
        if (talker.client < 0) {
            talker.client = idx; talker.ending = 0;
            memcpy(talker.session, hdr.session, 16);
            memcpy(talker.talk_id, hdr.talk_id, sizeof(talker.talk_id));
            talker.last_audio_ms = now;
            printf("active talker granted: id=%s\n", talker.talk_id); fflush(stdout);
        }
        if (talker.client != idx || memcmp(talker.session, hdr.session, 16) ||
            memcmp(talker.talk_id, hdr.talk_id, sizeof(talker.talk_id))) {
            if (++c->rejected == 1 || c->rejected % 50 == 0)
                fprintf(stderr, "audio/control ignored: id=%s active=%s\n", hdr.talk_id, talker.talk_id);
            rejected++; continue;
        }
        if (hdr.type == PKT_END) {
            if (!talker.ending) {
                talker.ending = 1; talker.end_seq = ntohl(hdr.seq);
                talker.release_ms = now + grace;
            } else if (talker.end_seq != ntohl(hdr.seq)) { invalid++; continue; }
            /* Duplicate END does not extend the grace period. */
        } else {
            if (talker.ending && ptt_seq_diff(ntohl(hdr.seq), talker.end_seq) >= 0) {
                rejected++; c->rejected++; continue;
            }
            talker.last_audio_ms = now;
        }
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (!clients[i].used || i == idx) continue;
            ssize_t sent = sendto(sock, buf, sizeof(hdr) + ntohs(hdr.len), 0,
                (struct sockaddr *)&clients[i].addr, sizeof(clients[i].addr));
            if (sent < 0) {
                if (++send_errors == 1 || send_errors % 50 == 0) perror("forward sendto");
            } else { forwarded++; c->forwarded++; }
        }
    }
    printf("summary: audio=%lu forwarded=%lu rejected=%lu invalid=%lu send_errors=%lu\n", audio, forwarded, rejected, invalid, send_errors);
    close(sock); return 0;
}
