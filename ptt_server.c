/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * ptt_client.c
 * Copyright (C) 2026 Resilience Theatre
 */

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
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define SERVER_PORT 5000
#define MAX_PACKET 1600
#define MAX_CLIENTS 128
#define PKT_IDLE 0
#define PKT_AUDIO 1
#define CLIENT_TIMEOUT_MS 30000
#define TALKER_HOLD_MS 1000
#define TALK_ID_MAX 31

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint16_t len;
    char talk_id[TALK_ID_MAX + 1];
} packet_hdr_t;

typedef struct {
    int used;
    struct sockaddr_in addr;
    long long last_seen_ms;
    unsigned long rx_idle_packets;
    unsigned long rx_audio_packets;
    unsigned long tx_audio_packets;
    char last_talk_id[TALK_ID_MAX + 1];
} client_t;

static volatile sig_atomic_t g_running = 1;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void on_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

static void addr_to_text(const struct sockaddr_in *addr, char *out, size_t out_sz) {
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
    snprintf(out, out_sz, "%s:%u", ip[0] ? ip : "?", (unsigned)ntohs(addr->sin_port));
}

static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_family == b->sin_family &&
           a->sin_port == b->sin_port &&
           a->sin_addr.s_addr == b->sin_addr.s_addr;
}

static const char *id_or_q(const char *id) {
    return (id && id[0]) ? id : "?";
}

static int find_client(client_t *clients, const struct sockaddr_in *addr) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].used && same_addr(&clients[i].addr, addr)) {
            return i;
        }
    }
    return -1;
}

static int add_client(client_t *clients, const struct sockaddr_in *addr) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i].used) {
            memset(&clients[i], 0, sizeof(clients[i]));
            clients[i].used = 1;
            clients[i].addr = *addr;
            clients[i].last_seen_ms = now_ms();
            return i;
        }
    }
    return -1;
}

static void remove_stale_clients(client_t *clients, int *active_talker) {
    long long now = now_ms();
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i].used) {
            continue;
        }
        if (now - clients[i].last_seen_ms > CLIENT_TIMEOUT_MS) {
            char who[64];
            addr_to_text(&clients[i].addr, who, sizeof(who));
            printf("[%lld] client timeout removed: %s id=%s (rx_idle=%lu rx_audio=%lu tx_audio=%lu)\n",
                   now,
                   who,
                   id_or_q(clients[i].last_talk_id),
                   clients[i].rx_idle_packets,
                   clients[i].rx_audio_packets,
                   clients[i].tx_audio_packets);
            fflush(stdout);
            clients[i].used = 0;
            if (*active_talker == i) {
                *active_talker = -1;
                printf("[%lld] active talker released due to timeout: %s id=%s\n", now, who, id_or_q(clients[i].last_talk_id));
                fflush(stdout);
            }
        }
    }
}

int main(void) {
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    int one = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
    }

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt(SO_RCVTIMEO)");
    }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = htonl(INADDR_ANY);
    srv.sin_port = htons(SERVER_PORT);

    if (bind(sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        perror("bind");
        close(sock);
        return 1;
    }

    client_t clients[MAX_CLIENTS];
    memset(clients, 0, sizeof(clients));

    int active_talker = -1;
    long long active_talker_last_audio_ms = 0;
    unsigned long total_rx_idle = 0;
    unsigned long total_rx_audio = 0;
    unsigned long total_forwarded = 0;

    printf("ptt_server listening on UDP port %d\n", SERVER_PORT);
    printf("protocol: type=0 idle, type=1 opus audio, talk_id field included on all packets\n");
    printf("policy: first client sending audio gets talker slot until %d ms of silence\n", TALKER_HOLD_MS);
    fflush(stdout);

    while (g_running) {
        uint8_t buf[MAX_PACKET];
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &src_len);
        long long now = now_ms();

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                remove_stale_clients(clients, &active_talker);
                if (active_talker >= 0 && now - active_talker_last_audio_ms > TALKER_HOLD_MS) {
                    char who[64];
                    addr_to_text(&clients[active_talker].addr, who, sizeof(who));
                    printf("[%lld] active talker released after silence: %s id=%s\n", now, who, id_or_q(clients[active_talker].last_talk_id));
                    fflush(stdout);
                    active_talker = -1;
                }
                continue;
            }
            perror("recvfrom");
            break;
        }

        if ((size_t)n < sizeof(packet_hdr_t)) {
            continue;
        }

        packet_hdr_t hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        hdr.talk_id[TALK_ID_MAX] = '\0';
        uint16_t len = ntohs(hdr.len);
        if ((size_t)n < sizeof(hdr) + len) {
            continue;
        }

        int idx = find_client(clients, &src);
        if (idx < 0) {
            idx = add_client(clients, &src);
            if (idx < 0) {
                fprintf(stderr, "client table full, dropping packet\n");
                continue;
            }
            char who[64];
            addr_to_text(&src, who, sizeof(who));
            printf("[%lld] new client registered: %s\n", now, who);
            fflush(stdout);
        }
        clients[idx].last_seen_ms = now;
        memcpy(clients[idx].last_talk_id, hdr.talk_id, strnlen(hdr.talk_id, TALK_ID_MAX));
		clients[idx].last_talk_id[TALK_ID_MAX] = '\0';
        clients[idx].last_talk_id[TALK_ID_MAX] = '\0';

        if (hdr.type == PKT_IDLE) {
            total_rx_idle++;
            clients[idx].rx_idle_packets++;
            if (clients[idx].rx_idle_packets == 1 || clients[idx].rx_idle_packets % 500 == 0) {
                char who[64];
                addr_to_text(&clients[idx].addr, who, sizeof(who));
                printf("[%lld] RX idle from %s id=%s (#%lu)\n", now, who, id_or_q(hdr.talk_id), clients[idx].rx_idle_packets);
                fflush(stdout);
            }
        } else if (hdr.type == PKT_AUDIO && len > 0) {
            total_rx_audio++;
            clients[idx].rx_audio_packets++;

            if (active_talker < 0) {
                active_talker = idx;
                active_talker_last_audio_ms = now;
                char who[64];
                addr_to_text(&clients[idx].addr, who, sizeof(who));
                printf("[%lld] active talker granted to %s id=%s\n", now, who, id_or_q(hdr.talk_id));
                fflush(stdout);
            }

            if (active_talker == idx) {
                active_talker_last_audio_ms = now;

                unsigned long forwarded_this_packet = 0;
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    if (!clients[i].used || i == idx) {
                        continue;
                    }
                    ssize_t sent = sendto(sock, buf, sizeof(hdr) + len, 0,
                                          (struct sockaddr *)&clients[i].addr,
                                          sizeof(clients[i].addr));
                    if (sent >= 0) {
                        clients[i].tx_audio_packets++;
                        total_forwarded++;
                        forwarded_this_packet++;
                    }
                }

                if (clients[idx].rx_audio_packets == 1 || clients[idx].rx_audio_packets % 50 == 0) {
                    char who[64];
                    addr_to_text(&clients[idx].addr, who, sizeof(who));
                    printf("[%lld] RX audio from active talker %s id=%s (#%lu, %u bytes opus), forwarded to %lu client(s)\n",
                           now,
                           who,
                           id_or_q(hdr.talk_id),
                           clients[idx].rx_audio_packets,
                           len,
                           forwarded_this_packet);
                    fflush(stdout);
                }
            } else {
                if (clients[idx].rx_audio_packets == 1 || clients[idx].rx_audio_packets % 50 == 0) {
                    char who[64], owner[64];
                    addr_to_text(&clients[idx].addr, who, sizeof(who));
                    addr_to_text(&clients[active_talker].addr, owner, sizeof(owner));
                    printf("[%lld] RX audio from %s id=%s ignored, active talker is %s id=%s\n",
                           now,
                           who,
                           id_or_q(hdr.talk_id),
                           owner,
                           id_or_q(clients[active_talker].last_talk_id));
                    fflush(stdout);
                }
            }
        } else {
            char who[64];
            addr_to_text(&clients[idx].addr, who, sizeof(who));
            printf("[%lld] unknown packet type=%u len=%u from %s id=%s ignored\n",
                   now, hdr.type, len, who, id_or_q(hdr.talk_id));
            fflush(stdout);
        }

        remove_stale_clients(clients, &active_talker);
        if (active_talker >= 0 && now - active_talker_last_audio_ms > TALKER_HOLD_MS) {
            char who[64];
            addr_to_text(&clients[active_talker].addr, who, sizeof(who));
            printf("[%lld] active talker released after silence: %s id=%s\n", now, who, id_or_q(clients[active_talker].last_talk_id));
            fflush(stdout);
            active_talker = -1;
        }
    }

    printf("summary: total_rx_idle=%lu total_rx_audio=%lu total_forwarded=%lu\n",
           total_rx_idle, total_rx_audio, total_forwarded);

    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].used) {
            char who[64];
            addr_to_text(&clients[i].addr, who, sizeof(who));
            printf("client %s id=%s: rx_idle=%lu rx_audio=%lu tx_audio=%lu\n",
                   who,
                   id_or_q(clients[i].last_talk_id),
                   clients[i].rx_idle_packets,
                   clients[i].rx_audio_packets,
                   clients[i].tx_audio_packets);
        }
    }

    close(sock);
    return 0;
}
