#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <gst/app/app.h>
#include <gst/gst.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sodium.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define SERVER_PORT 5000
#define LOCAL_PORT 0
#define MAX_PACKET 1600
#define PKT_IDLE 0
#define PKT_AUDIO 1
#define PKT_FLAG_ENCRYPTED 0x01
#define INPUT_SCAN_MAX 64
#define TALK_ID_MAX 31
#define NONCE_SIZE 24
#define ROOM_SALT_CONTEXT "udpptt-room-key-v1"

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t flags;
    uint16_t len;
    char talk_id[TALK_ID_MAX + 1];
    uint8_t nonce[NONCE_SIZE];
} packet_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t flags;
    char talk_id[TALK_ID_MAX + 1];
} packet_ad_t;

typedef struct {
    int sock;
    struct sockaddr_in server_addr;
    atomic_int running;
    atomic_int ptt_pressed;
    atomic_int suppress_playback;
    int ptt_enabled;
    int encrypt_enabled;
    char txid[TALK_ID_MAX + 1];
    char last_rx_talk_id[TALK_ID_MAX + 1];
    unsigned char room_key[crypto_aead_xchacha20poly1305_ietf_KEYBYTES];

    GstElement *capture_pipeline;
    GstElement *capture_sink;

    GstElement *playback_pipeline;
    GstElement *playback_src;

    atomic_ulong tx_audio_packets;
    atomic_ulong tx_idle_packets;
    atomic_ulong rx_audio_packets;
    atomic_ulong rx_played_packets;
    atomic_ulong rx_ignored_idle_packets;
    atomic_ulong rx_auth_fail_packets;

    pthread_t key_thread;
    pthread_t send_thread;
    pthread_t recv_thread;
} app_t;

static app_t g_app;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void msleep_int(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void on_sigint(int sig) {
    (void)sig;
    atomic_store(&g_app.running, 0);
}

static void sanitize_talk_id(const char *src, char out[TALK_ID_MAX + 1]) {
    size_t j = 0;
    if (!src || !src[0]) {
        snprintf(out, TALK_ID_MAX + 1, "anon");
        return;
    }
    for (size_t i = 0; src[i] && j < TALK_ID_MAX; ++i) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.') {
            out[j++] = (char)c;
        } else if (c == ' ') {
            out[j++] = '_';
        }
    }
    if (j == 0) {
        snprintf(out, TALK_ID_MAX + 1, "anon");
        return;
    }
    out[j] = '\0';
}

static int make_udp_socket(const char *server_ip, struct sockaddr_in *out_addr) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(LOCAL_PORT);

    if (bind(fd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    memset(out_addr, 0, sizeof(*out_addr));
    out_addr->sin_family = AF_INET;
    out_addr->sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, server_ip, &out_addr->sin_addr) != 1) {
        fprintf(stderr, "invalid server ip: %s\n", server_ip);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)out_addr, sizeof(*out_addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt(SO_RCVTIMEO)");
    }

    return fd;
}

static GstElement *make_capture_pipeline(GstElement **out_sink) {
    GError *err = NULL;
    const char *desc =
        "autoaudiosrc ! "
        "audio/x-raw,format=S16LE,channels=1,rate=48000 ! "
        "audioconvert ! audioresample ! "
        "opusenc frame-size=20 bitrate=24000 audio-type=voice ! "
        "appsink name=capture_sink emit-signals=false sync=false max-buffers=8 drop=true";

    GstElement *pipeline = gst_parse_launch(desc, &err);
    if (!pipeline) {
        fprintf(stderr, "capture pipeline error: %s\n", err ? err->message : "unknown");
        if (err) g_error_free(err);
        return NULL;
    }

    *out_sink = gst_bin_get_by_name(GST_BIN(pipeline), "capture_sink");
    if (!*out_sink) {
        fprintf(stderr, "failed to get capture_sink\n");
        gst_object_unref(pipeline);
        return NULL;
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    return pipeline;
}

static GstElement *make_playback_pipeline(GstElement **out_src) {
    GError *err = NULL;
    const char *desc =
        "appsrc name=play_src is-live=true format=time block=false do-timestamp=true ! "
        "opusdec ! audioconvert ! audioresample ! autoaudiosink";

    GstElement *pipeline = gst_parse_launch(desc, &err);
    if (!pipeline) {
        fprintf(stderr, "playback pipeline error: %s\n", err ? err->message : "unknown");
        if (err) g_error_free(err);
        return NULL;
    }

    *out_src = gst_bin_get_by_name(GST_BIN(pipeline), "play_src");
    if (!*out_src) {
        fprintf(stderr, "failed to get play_src\n");
        gst_object_unref(pipeline);
        return NULL;
    }

    GstCaps *caps = gst_caps_new_simple("audio/x-opus",
                                        "channel-mapping-family", G_TYPE_INT, 0,
                                        NULL);
    g_object_set(*out_src,
                 "caps", caps,
                 "stream-type", 0,
                 "format", GST_FORMAT_TIME,
                 NULL);
    gst_caps_unref(caps);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    return pipeline;
}

static void fill_packet_ad(packet_ad_t *ad, uint8_t type, uint8_t flags, const char *talk_id) {
    memset(ad, 0, sizeof(*ad));
    ad->type = type;
    ad->flags = flags;
    memcpy(ad->talk_id, talk_id, strnlen(talk_id, TALK_ID_MAX));
}

static int derive_room_key_from_password(const char *password, unsigned char key[crypto_aead_xchacha20poly1305_ietf_KEYBYTES]) {
    unsigned char salt[crypto_pwhash_SALTBYTES];
    memset(salt, 0, sizeof(salt));
    crypto_generichash(salt, sizeof(salt),
                       (const unsigned char *)ROOM_SALT_CONTEXT, strlen(ROOM_SALT_CONTEXT),
                       NULL, 0);
    return crypto_pwhash(key,
                         crypto_aead_xchacha20poly1305_ietf_KEYBYTES,
                         password,
                         strlen(password),
                         salt,
                         crypto_pwhash_OPSLIMIT_INTERACTIVE,
                         crypto_pwhash_MEMLIMIT_INTERACTIVE,
                         crypto_pwhash_ALG_DEFAULT);
}

static bool send_packet(app_t *app, uint8_t type, const uint8_t *payload, uint16_t len) {
    uint8_t buf[MAX_PACKET];
    uint8_t tmp[MAX_PACKET];
    packet_hdr_t hdr;
    packet_ad_t ad;
    size_t hdr_sz = sizeof(hdr);

    memset(&hdr, 0, sizeof(hdr));
    hdr.type = type;
    memcpy(hdr.talk_id, app->txid, strnlen(app->txid, TALK_ID_MAX));

    if (!app->encrypt_enabled) {
        if ((size_t)len + hdr_sz > sizeof(buf)) {
            fprintf(stderr, "packet too large: %u\n", len);
            return false;
        }
        hdr.flags = 0;
        hdr.len = htons(len);
        memcpy(buf, &hdr, hdr_sz);
        if (len > 0 && payload) {
            memcpy(buf + hdr_sz, payload, len);
        }
        if (send(app->sock, buf, hdr_sz + len, 0) < 0) {
            perror("send");
            return false;
        }
        return true;
    }

    fill_packet_ad(&ad, type, PKT_FLAG_ENCRYPTED, app->txid);
    randombytes_buf(hdr.nonce, sizeof(hdr.nonce));
    hdr.flags = PKT_FLAG_ENCRYPTED;

    unsigned long long clen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(tmp,
                                                   &clen,
                                                   payload,
                                                   len,
                                                   (const unsigned char *)&ad,
                                                   sizeof(ad),
                                                   NULL,
                                                   hdr.nonce,
                                                   app->room_key) != 0) {
        fprintf(stderr, "encryption failed\n");
        return false;
    }

    if (hdr_sz + clen > sizeof(buf)) {
        fprintf(stderr, "encrypted packet too large: %llu\n", clen);
        return false;
    }

    hdr.len = htons((uint16_t)clen);
    memcpy(buf, &hdr, hdr_sz);
    if (clen > 0) {
        memcpy(buf + hdr_sz, tmp, (size_t)clen);
    }

    if (send(app->sock, buf, hdr_sz + (size_t)clen, 0) < 0) {
        perror("send");
        return false;
    }
    return true;
}

static bool decode_payload(app_t *app,
                           const packet_hdr_t *hdr,
                           const uint8_t *wire_payload,
                           uint16_t wire_len,
                           uint8_t *out,
                           uint16_t *out_len) {
    if (!(hdr->flags & PKT_FLAG_ENCRYPTED)) {
        if (wire_len > 0) {
            memcpy(out, wire_payload, wire_len);
        }
        *out_len = wire_len;
        return true;
    }

    if (!app->encrypt_enabled) {
        return false;
    }

    packet_ad_t ad;
    fill_packet_ad(&ad, hdr->type, hdr->flags, hdr->talk_id);

    unsigned long long plain_len = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(out,
                                                   &plain_len,
                                                   NULL,
                                                   wire_payload,
                                                   wire_len,
                                                   (const unsigned char *)&ad,
                                                   sizeof(ad),
                                                   hdr->nonce,
                                                   app->room_key) != 0) {
        return false;
    }

    if (plain_len > UINT16_MAX) {
        return false;
    }
    *out_len = (uint16_t)plain_len;
    return true;
}

static bool play_opus_packet(app_t *app, const uint8_t *data, uint16_t len) {
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, len, NULL);
    if (!buffer) {
        return false;
    }

    gst_buffer_fill(buffer, 0, data, len);
    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(app->playback_src), buffer);
    return ret == GST_FLOW_OK;
}

static void *keyboard_thread_main(void *arg) {
    app_t *app = (app_t *)arg;
    int fds[INPUT_SCAN_MAX];
    int nfds = 0;

    for (int i = 0; i < INPUT_SCAN_MAX; ++i) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            fds[nfds++] = fd;
        }
    }

    if (nfds == 0) {
        fprintf(stderr, "keyboard debug: no /dev/input/event* readable\n");
        return NULL;
    }

    printf("keyboard debug: monitoring Right Alt / AltGr on %d input device(s)\n", nfds);
    fflush(stdout);

    while (atomic_load(&app->running)) {
        bool had_event = false;
        for (int i = 0; i < nfds; ++i) {
            struct input_event ev;
            ssize_t n;
            while ((n = read(fds[i], &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
                had_event = true;
                if (ev.type == EV_KEY && ev.code == KEY_RIGHTALT) {
                    int pressed = (ev.value != 0);
                    int old = atomic_exchange(&app->ptt_pressed, pressed);
                    atomic_store(&app->suppress_playback, pressed);
                    if (old != pressed) {
                        printf("[%lld] PTT %s (txid=%s)\n", now_ms(),
                               pressed ? "DOWN -> sending microphone audio" : "UP -> sending idle frames",
                               app->txid);
                        fflush(stdout);
                    }
                }
            }
        }
        if (!had_event) {
            msleep_int(10);
        }
    }

    for (int i = 0; i < nfds; ++i) {
        close(fds[i]);
    }
    return NULL;
}

static void *send_thread_main(void *arg) {
    app_t *app = (app_t *)arg;
    unsigned long last_report_audio = 0;
    unsigned long last_report_idle = 0;

    while (atomic_load(&app->running)) {
        if (atomic_load(&app->ptt_pressed)) {
            GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(app->capture_sink), 50 * GST_MSECOND);
            if (sample) {
                GstBuffer *buffer = gst_sample_get_buffer(sample);
                GstMapInfo map;
                if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                    if (send_packet(app, PKT_AUDIO, map.data, (uint16_t)map.size)) {
                        unsigned long n = atomic_fetch_add(&app->tx_audio_packets, 1) + 1;
                        if (n == 1 || n - last_report_audio >= 50) {
                            printf("[%lld] TX mic audio packet #%lu (%zu bytes opus, txid=%s%s)\n",
                                   now_ms(), n, map.size, app->txid,
                                   app->encrypt_enabled ? ", enc=xchacha20poly1305" : "");
                            fflush(stdout);
                            last_report_audio = n;
                        }
                    }
                    gst_buffer_unmap(buffer, &map);
                }
                gst_sample_unref(sample);
            } else {
                if (send_packet(app, PKT_IDLE, NULL, 0)) {
                    atomic_fetch_add(&app->tx_idle_packets, 1);
                }
                msleep_int(20);
            }
        } else {
            if (send_packet(app, PKT_IDLE, NULL, 0)) {
                unsigned long n = atomic_fetch_add(&app->tx_idle_packets, 1) + 1;
                if (n == 1 || n - last_report_idle >= 200) {
                    printf("[%lld] TX idle packet #%lu (txid=%s%s)\n",
                           now_ms(), n, app->txid,
                           app->encrypt_enabled ? ", enc=xchacha20poly1305" : "");
                    fflush(stdout);
                    last_report_idle = n;
                }
            }
            msleep_int(20);
        }
    }

    return NULL;
}

static void *recv_thread_main(void *arg) {
    app_t *app = (app_t *)arg;
    uint8_t buf[MAX_PACKET];
    uint8_t plain[MAX_PACKET];
    unsigned long last_report_rx = 0;
    unsigned long last_report_play = 0;

    while (atomic_load(&app->running)) {
        ssize_t n = recv(app->sock, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            perror("recv");
            break;
        }
        if ((size_t)n < sizeof(packet_hdr_t)) {
            continue;
        }

        packet_hdr_t hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        hdr.talk_id[TALK_ID_MAX] = '\0';
        uint16_t wire_len = ntohs(hdr.len);
        if ((size_t)n < sizeof(hdr) + wire_len) {
            continue;
        }

        uint16_t plain_len = 0;
        if (!decode_payload(app, &hdr, buf + sizeof(hdr), wire_len, plain, &plain_len)) {
            unsigned long bad = atomic_fetch_add(&app->rx_auth_fail_packets, 1) + 1;
            if (bad == 1 || bad % 50 == 0) {
                printf("[%lld] RX auth/decrypt failed #%lu (from=%s, flags=0x%02x)\n",
                       now_ms(), bad, hdr.talk_id[0] ? hdr.talk_id : "?", hdr.flags);
                fflush(stdout);
            }
            continue;
        }

        if (strcmp(app->last_rx_talk_id, hdr.talk_id) != 0 && hdr.talk_id[0]) {
            snprintf(app->last_rx_talk_id, sizeof(app->last_rx_talk_id), "%s", hdr.talk_id);
            printf("[%lld] TALKER now: %s\n", now_ms(), app->last_rx_talk_id);
            fflush(stdout);
        }

        if (hdr.type == PKT_IDLE) {
            unsigned long idle_n = atomic_fetch_add(&app->rx_ignored_idle_packets, 1) + 1;
            if (idle_n == 1 || idle_n % 200 == 0) {
                printf("[%lld] RX idle packet ignored #%lu (from=%s%s)\n",
                       now_ms(), idle_n, hdr.talk_id[0] ? hdr.talk_id : "?",
                       (hdr.flags & PKT_FLAG_ENCRYPTED) ? ", encrypted" : "");
                fflush(stdout);
            }
            continue;
        }

        if (hdr.type != PKT_AUDIO || plain_len == 0) {
            continue;
        }

        unsigned long rx_n = atomic_fetch_add(&app->rx_audio_packets, 1) + 1;
        if (rx_n == 1 || rx_n - last_report_rx >= 50) {
            printf("[%lld] RX audio packet #%lu (%u bytes opus, from=%s%s)\n",
                   now_ms(), rx_n, plain_len, hdr.talk_id[0] ? hdr.talk_id : "?",
                   (hdr.flags & PKT_FLAG_ENCRYPTED) ? ", encrypted" : "");
            fflush(stdout);
            last_report_rx = rx_n;
        }

        if (atomic_load(&app->suppress_playback)) {
            if (rx_n == 1 || rx_n % 50 == 0) {
                printf("[%lld] RX audio suppressed while local PTT is active (from=%s)\n",
                       now_ms(), hdr.talk_id[0] ? hdr.talk_id : "?");
                fflush(stdout);
            }
            continue;
        }

        if (play_opus_packet(app, plain, plain_len)) {
            unsigned long play_n = atomic_fetch_add(&app->rx_played_packets, 1) + 1;
            if (play_n == 1 || play_n - last_report_play >= 50) {
                printf("[%lld] PLAY audio packet #%lu (from=%s)\n",
                       now_ms(), play_n, hdr.talk_id[0] ? hdr.talk_id : "?");
                fflush(stdout);
                last_report_play = play_n;
            }
        }
    }

    return NULL;
}

static void cleanup(app_t *app) {
    atomic_store(&app->running, 0);

    if (app->key_thread) pthread_join(app->key_thread, NULL);
    if (app->send_thread) pthread_join(app->send_thread, NULL);
    if (app->recv_thread) pthread_join(app->recv_thread, NULL);

    if (app->capture_pipeline) {
        gst_element_set_state(app->capture_pipeline, GST_STATE_NULL);
        if (app->capture_sink) gst_object_unref(app->capture_sink);
        gst_object_unref(app->capture_pipeline);
    }

    if (app->playback_pipeline) {
        gst_element_set_state(app->playback_pipeline, GST_STATE_NULL);
        if (app->playback_src) {
            gst_app_src_end_of_stream(GST_APP_SRC(app->playback_src));
            gst_object_unref(app->playback_src);
        }
        gst_object_unref(app->playback_pipeline);
    }

    if (app->sock >= 0) {
        close(app->sock);
        app->sock = -1;
    }

    sodium_memzero(app->room_key, sizeof(app->room_key));

    printf("summary: tx_audio=%lu tx_idle=%lu rx_audio=%lu played=%lu rx_idle_ignored=%lu rx_auth_fail=%lu\n",
           (unsigned long)atomic_load(&app->tx_audio_packets),
           (unsigned long)atomic_load(&app->tx_idle_packets),
           (unsigned long)atomic_load(&app->rx_audio_packets),
           (unsigned long)atomic_load(&app->rx_played_packets),
           (unsigned long)atomic_load(&app->rx_ignored_idle_packets),
           (unsigned long)atomic_load(&app->rx_auth_fail_packets));
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s <server-ip> [--txid NAME] [--rx-only|--no-ptt] [--encrypt] [--key PASSWORD]\n"
            "       env: UDPPTT_KEY=PASSWORD (used when --encrypt is set and --key is not)\n",
            prog);
}

int main(int argc, char **argv) {
    const char *server_ip = NULL;
    const char *password = NULL;

    memset(&g_app, 0, sizeof(g_app));
    g_app.sock = -1;
    g_app.ptt_enabled = 1;
    sanitize_talk_id("anon", g_app.txid);

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--rx-only") == 0 || strcmp(argv[i], "--no-ptt") == 0) {
            g_app.ptt_enabled = 0;
        } else if (strcmp(argv[i], "--encrypt") == 0) {
            g_app.encrypt_enabled = 1;
        } else if (strcmp(argv[i], "--txid") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                return 1;
            }
            sanitize_talk_id(argv[i], g_app.txid);
        } else if (strcmp(argv[i], "--key") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                return 1;
            }
            password = argv[i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        } else if (!server_ip) {
            server_ip = argv[i];
        } else {
            fprintf(stderr, "unexpected argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!server_ip) {
        usage(argv[0]);
        return 1;
    }

    if (sodium_init() < 0) {
        fprintf(stderr, "sodium_init failed\n");
        return 1;
    }

    if (g_app.encrypt_enabled) {
        if (!password || !password[0]) {
            password = getenv("UDPPTT_KEY");
        }
        if (!password || !password[0]) {
            fprintf(stderr, "--encrypt requires --key PASSWORD or UDPPTT_KEY in environment\n");
            return 1;
        }
        if (derive_room_key_from_password(password, g_app.room_key) != 0) {
            fprintf(stderr, "failed to derive encryption key from password\n");
            return 1;
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    gst_init(&argc, &argv);

    g_app.sock = make_udp_socket(server_ip, &g_app.server_addr);
    if (g_app.sock < 0) {
        cleanup(&g_app);
        return 1;
    }

    if (g_app.ptt_enabled) {
        g_app.capture_pipeline = make_capture_pipeline(&g_app.capture_sink);
        if (!g_app.capture_pipeline) {
            cleanup(&g_app);
            return 1;
        }
    }

    g_app.playback_pipeline = make_playback_pipeline(&g_app.playback_src);
    if (!g_app.playback_pipeline) {
        cleanup(&g_app);
        return 1;
    }

    atomic_store(&g_app.running, 1);
    atomic_store(&g_app.ptt_pressed, 0);
    atomic_store(&g_app.suppress_playback, 0);

    printf("connected to %s:%d\n", server_ip, SERVER_PORT);
    printf("txid: %s\n", g_app.txid);
    printf("debug: shows PTT state, transmitted mic packets, received audio packets, playback events, and talker IDs\n");
    if (!g_app.ptt_enabled) {
        printf("mode: receive-only (--rx-only), keyboard PTT disabled, microphone capture disabled\n");
    }
    if (g_app.encrypt_enabled) {
        printf("encryption: enabled (XChaCha20-Poly1305 AEAD, talk_id cleartext but authenticated)\n");
    } else {
        printf("encryption: disabled\n");
    }
    fflush(stdout);

    if (g_app.ptt_enabled) {
        if (pthread_create(&g_app.key_thread, NULL, keyboard_thread_main, &g_app) != 0) {
            perror("pthread_create(key_thread)");
            cleanup(&g_app);
            return 1;
        }
    }
    if (pthread_create(&g_app.send_thread, NULL, send_thread_main, &g_app) != 0) {
        perror("pthread_create(send_thread)");
        cleanup(&g_app);
        return 1;
    }
    if (pthread_create(&g_app.recv_thread, NULL, recv_thread_main, &g_app) != 0) {
        perror("pthread_create(recv_thread)");
        cleanup(&g_app);
        return 1;
    }

    while (atomic_load(&g_app.running)) {
        msleep_int(100);
    }

    cleanup(&g_app);
    return 0;
}
