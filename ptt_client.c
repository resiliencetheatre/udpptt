/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <gst/app/app.h>
#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <gst/base/gstadapter.h>
#include <math.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/input.h>
#include <limits.h>
#include <net/if.h>
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
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "ptt_protocol.h"
#include "ptt_jitter.h"

#define DEFAULT_SERVER_PORT 5000
#define DEFAULT_BLACKFIBER_ETHERTYPE 0x88B5
#define LOCAL_PORT 0
#define MAX_FRAME (MAX_PACKET + 128)
#define INPUT_SCAN_MAX 64
#define KEY_LEN crypto_aead_xchacha20poly1305_ietf_KEYBYTES
#define ALSA_DEV_MAX 128

typedef enum {
    TRANSPORT_UDP = 0,
    TRANSPORT_BLACKFIBER = 1
} transport_mode_t;

typedef struct __attribute__((packed)) {
    uint8_t dst[ETH_ALEN];
    uint8_t src[ETH_ALEN];
    uint16_t ethertype;
} bf_eth_hdr_t;

typedef struct {
    char wav_path[PATH_MAX];
    char playback_device[ALSA_DEV_MAX];
} tone_play_req_t;

typedef struct {
    int sock;
    int server_port;
    int ptt_ctrl_sock;
    int ptt_socket_enabled;
    char ptt_socket_path[PATH_MAX];
    int state_file_enabled;
    char state_file_path[PATH_MAX];
    int rx_state_timeout_ms;
    int jitter_ms, loss_percent, fec_enabled;
    uint8_t tx_session[16];
    uint32_t tx_seq;
    atomic_uint ptt_generation;
    pthread_mutex_t jitter_lock;
    ptt_jitter_t jitter;
    pthread_t play_thread;
    atomic_ulong rx_invalid_packets, playback_errors, playback_warnings;
    atomic_ulong playback_starvations, playback_queue_drops, playback_schedule_late;
    atomic_ulong rx_suppressed_packets;
    atomic_ulong tx_capture_gaps;
    pthread_mutex_t state_lock;
    int state_tx_pressed;
    int state_rx_active;
    char state_rx_talker[TALK_ID_MAX + 1];
    long long state_rx_last_audio_ms;
    transport_mode_t transport_mode;
    int blackfiber_rx_passive;
    int blackfiber_tx_only;
    struct sockaddr_in server_addr;
    char bf_ifname[IFNAMSIZ];
    uint16_t bf_ethertype;
    int bf_ifindex;
    uint8_t bf_local_mac[ETH_ALEN];
    uint8_t bf_dst_mac[ETH_ALEN];
    atomic_int running;
    atomic_int ptt_pressed;
    atomic_int ptt_keyboard_pressed;
    atomic_int ptt_socket_pressed;
    atomic_int suppress_playback;
    int ptt_enabled;
    int encrypt_enabled;
    int codec_ptt_enabled;
    int pc_ptt_hold_ms;
    char txid[TALK_ID_MAX + 1];
    unsigned char key[KEY_LEN];
    char alsa_capture_device[ALSA_DEV_MAX];
    char alsa_playback_device[ALSA_DEV_MAX];
    int preamble_enabled;
    tm_record telemetry_tx;
    char gps_file[PATH_MAX];
    char telemetry_socket[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int telemetry_fd;
    atomic_uint playback_quiesced;
    atomic_int microphone_ready;
    atomic_int monitor_active;
    int have_start_wav;
    int have_stop_wav;
    char start_wav_path[PATH_MAX];
    char stop_wav_path[PATH_MAX];
    GstElement *capture_pipeline;
    GstElement *capture_sink;
    GstElement *playback_pipeline;
    GstElement *playback_src;
    atomic_ulong tx_audio_packets;
    atomic_ulong tx_idle_packets;
    atomic_ulong rx_audio_packets;
    atomic_ulong rx_played_packets;
    atomic_ulong rx_ignored_idle_packets;
    atomic_ulong rx_decrypt_failures;
    pthread_t key_thread;
    pthread_t ctrl_thread;
    pthread_t send_thread;
    pthread_t recv_thread;
} app_t;

static app_t g_app;
static const unsigned char g_pwhash_salt[crypto_pwhash_SALTBYTES] = {
    0x75, 0x64, 0x70, 0x70, 0x74, 0x74, 0x2d, 0x78,
    0x63, 0x68, 0x61, 0x63, 0x68, 0x61, 0x32, 0x30
};

static long long now_ms(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL; }
static int64_t mono_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
static void msleep_int(int ms) { struct timespec ts; ts.tv_sec = ms / 1000; ts.tv_nsec = (ms % 1000) * 1000000L; nanosleep(&ts, NULL); }
static void on_sigint(int sig) { (void)sig; atomic_store(&g_app.running, 0); }

static void sanitize_txid(char *dst, size_t dst_sz, const char *src) {
    size_t j = 0;
    if (!src || !src[0]) { snprintf(dst, dst_sz, "anon"); return; }
    for (size_t i = 0; src[i] != '\0' && j + 1 < dst_sz; ++i) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') dst[j++] = (char)c;
        else if (c == ' ') dst[j++] = '_';
    }
    if (j == 0) snprintf(dst, dst_sz, "anon"); else dst[j] = '\0';
}
static void copy_opt_string(char *dst, size_t dst_sz, const char *src) { if (!dst || dst_sz == 0) return; memset(dst, 0, dst_sz); if (!src) return; snprintf(dst, dst_sz, "%s", src); }
static int parse_udp_port(const char *s) { char *endp = NULL; long v; if (!s || !s[0]) return -1; errno = 0; v = strtol(s, &endp, 10); if (errno || !endp || *endp != '\0' || v < 1 || v > 65535) return -1; return (int)v; }
static int parse_ethertype(const char *s, uint16_t *out) { char *endp = NULL; long v; if (!s || !s[0] || !out) return -1; errno = 0; v = strtol(s, &endp, 0); if (errno || !endp || *endp != '\0' || v < 0x0600 || v > 0xFFFF) return -1; *out = (uint16_t)v; return 0; }

static int parse_mac_address(const char *s, uint8_t out[ETH_ALEN]) {
    unsigned int b[ETH_ALEN];
    if (!s || !out) return -1;
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return -1;
    for (int i = 0; i < ETH_ALEN; ++i) out[i] = (uint8_t)b[i];
    return 0;
}

static void format_mac(char *dst, size_t dst_sz, const uint8_t mac[ETH_ALEN]) {
    if (!dst || dst_sz == 0 || !mac) return;
    snprintf(dst, dst_sz, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int mkdir_parent_dir_for_file(const char *path) {
    char tmp[PATH_MAX];
    char *slash;

    if (!path || !path[0]) return -1;
    if (strlen(path) >= sizeof(tmp)) return -1;

    snprintf(tmp, sizeof(tmp), "%s", path);
    slash = strrchr(tmp, '/');
    if (!slash) return 0;
    if (slash == tmp) return 0;
    *slash = '\0';

    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }

    if (mkdir(tmp, 0700) < 0 && errno != EEXIST) return -1;
    return 0;
}

static int write_all_exact(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void write_state_file_locked(app_t *app) {
    char tmp_path[PATH_MAX];
    char content[512];
    int fd;
    int n;
    long long t;

    if (!app || !app->state_file_enabled || !app->state_file_path[0]) return;

    if (mkdir_parent_dir_for_file(app->state_file_path) != 0) {
        fprintf(stderr, "state-file: failed to create parent directory for %s: %s\n", app->state_file_path, strerror(errno));
        return;
    }

    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", app->state_file_path, (long)getpid()) >= (int)sizeof(tmp_path)) {
        fprintf(stderr, "state-file: temporary path too long for %s\n", app->state_file_path);
        return;
    }

    t = now_ms();
    n = snprintf(content, sizeof(content),
                 "PTT %s txid=%s time_ms=%lld\n"
                 "tx=%d\n"
                 "rx=%d\n"
                 "txid=%s\n"
                 "talker=%s\n"
                 "time_ms=%lld\n"
                 "rx_timeout_ms=%d\n"
                 "rx_last_audio_ms=%lld\n",
                 app->state_tx_pressed ? "DOWN" : "UP",
                 app->txid,
                 t,
                 app->state_tx_pressed ? 1 : 0,
                 app->state_rx_active ? 1 : 0,
                 app->txid,
                 app->state_rx_active ? app->state_rx_talker : "",
                 t,
                 app->rx_state_timeout_ms,
                 app->state_rx_last_audio_ms);
    if (n < 0 || (size_t)n >= sizeof(content)) return;

    fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "state-file: open %s failed: %s\n", tmp_path, strerror(errno));
        return;
    }

    if (write_all_exact(fd, content, (size_t)n) != 0) {
        fprintf(stderr, "state-file: write %s failed: %s\n", tmp_path, strerror(errno));
        close(fd);
        unlink(tmp_path);
        return;
    }

    if (fsync(fd) < 0) {
        fprintf(stderr, "state-file: fsync %s failed: %s\n", tmp_path, strerror(errno));
    }

    close(fd);

    if (rename(tmp_path, app->state_file_path) < 0) {
        fprintf(stderr, "state-file: rename %s -> %s failed: %s\n", tmp_path, app->state_file_path, strerror(errno));
        unlink(tmp_path);
        return;
    }
}

static void state_file_set_tx(app_t *app, int pressed) {
    if (!app || !app->state_file_enabled) return;
    pthread_mutex_lock(&app->state_lock);
    app->state_tx_pressed = pressed ? 1 : 0;
    write_state_file_locked(app);
    pthread_mutex_unlock(&app->state_lock);
}

static void state_file_note_rx_audio(app_t *app, const char *talker) {
    int changed = 0;
    long long t;

    if (!app || !app->state_file_enabled) return;

    t = now_ms();
    pthread_mutex_lock(&app->state_lock);
    app->state_rx_last_audio_ms = t;

    if (!app->state_rx_active) {
        app->state_rx_active = 1;
        changed = 1;
    }

    if (!talker) talker = "";
    if (strncmp(app->state_rx_talker, talker, TALK_ID_MAX) != 0) {
        snprintf(app->state_rx_talker, sizeof(app->state_rx_talker), "%s", talker);
        changed = 1;
    }

    if (changed) write_state_file_locked(app);
    pthread_mutex_unlock(&app->state_lock);
}

static void state_file_check_rx_timeout(app_t *app) {
    long long t;

    if (!app || !app->state_file_enabled || app->rx_state_timeout_ms <= 0) return;

    t = now_ms();
    pthread_mutex_lock(&app->state_lock);
    if (app->state_rx_active && app->state_rx_last_audio_ms > 0 && t - app->state_rx_last_audio_ms >= app->rx_state_timeout_ms) {
        app->state_rx_active = 0;
        app->state_rx_talker[0] = '\0';
        write_state_file_locked(app);
    }
    pthread_mutex_unlock(&app->state_lock);
}

static void state_file_write_idle(app_t *app) {
    if (!app || !app->state_file_enabled) return;
    pthread_mutex_lock(&app->state_lock);
    app->state_tx_pressed = 0;
    app->state_rx_active = 0;
    app->state_rx_talker[0] = '\0';
    app->state_rx_last_audio_ms = 0;
    write_state_file_locked(app);
    pthread_mutex_unlock(&app->state_lock);
}

static int derive_key_from_password(const char *password, unsigned char out_key[KEY_LEN]) {
    if (!password || !password[0]) return -1;
    if (crypto_pwhash(out_key, KEY_LEN, password, strlen(password), g_pwhash_salt, crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE, crypto_pwhash_ALG_DEFAULT) != 0) {
        fprintf(stderr, "crypto_pwhash failed (out of memory?)\n"); return -1;
    }
    return 0;
}

static int make_udp_socket(const char *server_ip, int server_port, struct sockaddr_in *out_addr) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0); if (fd < 0) { perror("socket"); return -1; }
    struct sockaddr_in local_addr; memset(&local_addr, 0, sizeof(local_addr)); local_addr.sin_family = AF_INET; local_addr.sin_addr.s_addr = htonl(INADDR_ANY); local_addr.sin_port = htons(LOCAL_PORT);
    if (bind(fd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) { perror("bind"); close(fd); return -1; }
    memset(out_addr, 0, sizeof(*out_addr)); out_addr->sin_family = AF_INET; out_addr->sin_port = htons((uint16_t)server_port);
    if (inet_pton(AF_INET, server_ip, &out_addr->sin_addr) != 1) { fprintf(stderr, "invalid server ip: %s\n", server_ip); close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)out_addr, sizeof(*out_addr)) < 0) { perror("connect"); close(fd); return -1; }
    struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 200000; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); return fd;
}

static int make_blackfiber_socket(app_t *app) {
    int fd;
    struct ifreq ifr;
    struct sockaddr_ll sll;
    struct timeval tv;

    if (!app || !app->bf_ifname[0]) {
        fprintf(stderr, "blackfiber mode requires interface name\n");
        return -1;
    }

    fd = socket(AF_PACKET, SOCK_RAW, htons(app->bf_ethertype));
    if (fd < 0) { perror("socket(AF_PACKET)"); return -1; }

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", app->bf_ifname);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) { perror("ioctl(SIOCGIFINDEX)"); close(fd); return -1; }
    app->bf_ifindex = ifr.ifr_ifindex;

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", app->bf_ifname);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) { perror("ioctl(SIOCGIFHWADDR)"); close(fd); return -1; }
    memcpy(app->bf_local_mac, ifr.ifr_hwaddr.sa_data, ETH_ALEN);

    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(app->bf_ethertype);
    sll.sll_ifindex = app->bf_ifindex;

    if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) { perror("bind(AF_PACKET)"); close(fd); return -1; }

    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static GstElement *make_capture_pipeline(GstElement **out_sink, const char *alsa_device, int fec, int loss) {
    GError *err = NULL; char desc[768];
    if (alsa_device && alsa_device[0]) snprintf(desc, sizeof(desc), "alsasrc device=\"%s\" ! audio/x-raw,format=S16LE,channels=1,rate=48000 ! audioconvert ! audioresample ! opusenc frame-size=20 bitrate=24000 audio-type=voice inband-fec=%s packet-loss-percentage=%d ! appsink name=capture_sink emit-signals=false sync=false max-buffers=8 drop=true", alsa_device, fec ? "true" : "false", loss);
    else snprintf(desc, sizeof(desc), "autoaudiosrc ! audio/x-raw,format=S16LE,channels=1,rate=48000 ! audioconvert ! audioresample ! opusenc frame-size=20 bitrate=24000 audio-type=voice inband-fec=%s packet-loss-percentage=%d ! appsink name=capture_sink emit-signals=false sync=false max-buffers=8 drop=true", fec ? "true" : "false", loss);
    GstElement *pipeline = gst_parse_launch(desc, &err);
    if (!pipeline || err) { fprintf(stderr, "capture pipeline error: %s\n", err ? err->message : "unknown"); if (err) g_error_free(err); if (pipeline) gst_object_unref(pipeline); return NULL; }
    *out_sink = gst_bin_get_by_name(GST_BIN(pipeline), "capture_sink"); if (!*out_sink) { fprintf(stderr, "failed to get capture_sink\n"); gst_object_unref(pipeline); return NULL; }
    gst_element_set_state(pipeline, GST_STATE_PLAYING); return pipeline;
}

static GstElement *make_playback_pipeline(GstElement **out_src, const char *alsa_device) {
    GError *err = NULL; char desc[512];
    if (alsa_device && alsa_device[0]) snprintf(desc, sizeof(desc), "appsrc name=play_src is-live=true format=time block=false do-timestamp=true max-buffers=5 max-bytes=0 max-time=0 leaky-type=downstream ! audioconvert ! audioresample ! alsasink device=\"%s\" sync=false", alsa_device);
    else snprintf(desc, sizeof(desc), "appsrc name=play_src is-live=true format=time block=false do-timestamp=true max-buffers=5 max-bytes=0 max-time=0 leaky-type=downstream ! audioconvert ! audioresample ! autoaudiosink sync=false");
    GstElement *pipeline = gst_parse_launch(desc, &err);
    if (!pipeline || err) { fprintf(stderr, "playback pipeline error: %s\n", err ? err->message : "unknown"); if (err) g_error_free(err); if (pipeline) gst_object_unref(pipeline); return NULL; }
    *out_src = gst_bin_get_by_name(GST_BIN(pipeline), "play_src"); if (!*out_src) { fprintf(stderr, "failed to get play_src\n"); gst_object_unref(pipeline); return NULL; }
    GstCaps *caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, GST_AUDIO_NE(S16), "layout", G_TYPE_STRING, "interleaved", "rate", G_TYPE_INT, 48000, "channels", G_TYPE_INT, 1, NULL);
    g_object_set(*out_src, "caps", caps, "stream-type", 0, "format", GST_FORMAT_TIME, NULL); gst_caps_unref(caps);
    gst_element_set_state(pipeline, GST_STATE_PLAYING); return pipeline;
}

static bool send_packet(app_t *app, uint8_t type, const uint8_t *payload, uint16_t plain_len) {
    packet_hdr_t hdr;
    unsigned long long wire_len = plain_len;
    const uint8_t *wire_payload = payload;
    unsigned char cipher[MAX_PACKET];

    ptt_header_init(&hdr, type);
    if (type != PKT_IDLE) {
        memcpy(hdr.session, app->tx_session, sizeof(hdr.session));
        hdr.seq = htonl(app->tx_seq);
        hdr.timestamp = htonl(app->tx_seq * (uint32_t)PTT_SAMPLES);
        if (type == PKT_AUDIO) { hdr.samples = htons(PTT_SAMPLES); app->tx_seq++; }
    }
    hdr.len = htons(plain_len + (app->encrypt_enabled ? PTT_TAG_BYTES : 0));
    memcpy(hdr.talk_id, app->txid, strnlen(app->txid, TALK_ID_MAX + 1));

    if (app->encrypt_enabled) {
        hdr.flags |= PKT_FLAG_ENCRYPTED;
        randombytes_buf(hdr.nonce, sizeof(hdr.nonce));
        if (plain_len + crypto_aead_xchacha20poly1305_ietf_ABYTES > sizeof(cipher)) { fprintf(stderr, "encrypted packet too large: %u\n", plain_len); return false; }
        if (crypto_aead_xchacha20poly1305_ietf_encrypt(cipher, &wire_len, payload, plain_len, (const unsigned char *)&hdr, sizeof(hdr), NULL, hdr.nonce, app->key) != 0) { fprintf(stderr, "encrypt failed\n"); return false; }
        wire_payload = cipher;
    }

    if (app->transport_mode == TRANSPORT_UDP) {
        uint8_t buf[MAX_PACKET];
        if (sizeof(hdr) + wire_len > sizeof(buf)) { fprintf(stderr, "packet too large: %llu\n", wire_len); return false; }
        hdr.len = htons((uint16_t)wire_len);
        memcpy(buf, &hdr, sizeof(hdr));
        if (wire_len > 0 && wire_payload) memcpy(buf + sizeof(hdr), wire_payload, (size_t)wire_len);
        ssize_t sent = send(app->sock, buf, sizeof(hdr) + (size_t)wire_len, 0); if (sent < 0) { perror("send"); return false; } return true;
    } else {
        uint8_t frame[MAX_FRAME];
        bf_eth_hdr_t *eh = (bf_eth_hdr_t *)frame;
        struct sockaddr_ll sll;
        size_t frame_len;

        if (sizeof(*eh) + sizeof(hdr) + wire_len > sizeof(frame)) { fprintf(stderr, "blackfiber frame too large: %llu\n", wire_len); return false; }

        memcpy(eh->dst, app->bf_dst_mac, ETH_ALEN);
        memcpy(eh->src, app->bf_local_mac, ETH_ALEN);
        eh->ethertype = htons(app->bf_ethertype);

        hdr.len = htons((uint16_t)wire_len);
        memcpy(frame + sizeof(*eh), &hdr, sizeof(hdr));
        if (wire_len > 0 && wire_payload) memcpy(frame + sizeof(*eh) + sizeof(hdr), wire_payload, (size_t)wire_len);
        frame_len = sizeof(*eh) + sizeof(hdr) + (size_t)wire_len;

        memset(&sll, 0, sizeof(sll));
        sll.sll_family = AF_PACKET;
        sll.sll_ifindex = app->bf_ifindex;
        sll.sll_halen = ETH_ALEN;
        memcpy(sll.sll_addr, app->bf_dst_mac, ETH_ALEN);

        ssize_t sent = sendto(app->sock, frame, frame_len, 0, (struct sockaddr *)&sll, sizeof(sll));
        if (sent < 0) { perror("sendto(AF_PACKET)"); return false; }
        return true;
    }
}

static void report_rx_stats(app_t *app) {
    pthread_mutex_lock(&app->jitter_lock);
    ptt_rx_stats_t r = app->jitter.stats;
    unsigned long long telemetry_drops = app->jitter.event_drops;
    pthread_mutex_unlock(&app->jitter_lock);
    printf("audio stats: accepted=%llu decoded=%llu missing=%llu fec_attempts=%llu plc=%llu late=%llu duplicate=%llu reordered=%llu invalid=%llu overflow=%llu rebuffer=%llu timeouts=%llu jitter_ms=%.1f suppressed=%lu queue_drops=%lu scheduling_late=%lu audio_errors=%lu audio_warnings=%lu stream_failed_warnings=%lu capture_gaps=%lu tx_audio=%lu rx_audio=%lu auth_fail=%lu telemetry_drops=%llu\n",
        (unsigned long long)r.accepted, (unsigned long long)r.decoded,
        (unsigned long long)r.missing, (unsigned long long)r.fec_attempts,
        (unsigned long long)r.plc, (unsigned long long)r.late,
        (unsigned long long)r.duplicates, (unsigned long long)r.reordered,
        (unsigned long long)(r.invalid + atomic_load(&app->rx_invalid_packets)),
        (unsigned long long)r.overflow, (unsigned long long)r.rebuffered,
        (unsigned long long)r.timeouts, r.jitter_ms,
        atomic_load(&app->rx_suppressed_packets), atomic_load(&app->playback_queue_drops),
        atomic_load(&app->playback_schedule_late), atomic_load(&app->playback_errors),
        atomic_load(&app->playback_warnings), atomic_load(&app->playback_starvations),
        atomic_load(&app->tx_capture_gaps), atomic_load(&app->tx_audio_packets),
        atomic_load(&app->rx_audio_packets), atomic_load(&app->rx_decrypt_failures), telemetry_drops);
    fflush(stdout);
}

static void check_audio_bus(app_t *app, GstElement *pipeline) {
    if (!pipeline) return;
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg;
    while ((msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR | GST_MESSAGE_WARNING))) {
        GError *err = NULL; gchar *debug = NULL;
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            gst_message_parse_error(msg, &err, &debug);
            atomic_fetch_add(&app->playback_errors, 1);
        } else {
            gst_message_parse_warning(msg, &err, &debug);
            atomic_fetch_add(&app->playback_warnings, 1);
            if (err && err->domain == GST_STREAM_ERROR && err->code == GST_STREAM_ERROR_FAILED)
                atomic_fetch_add(&app->playback_starvations, 1);
        }
        fprintf(stderr, "audio %s (%s): %s [%s]\n",
            GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR ? "error" : "warning",
            GST_OBJECT_NAME(msg->src), err ? err->message : "unknown", debug ? debug : "");
        g_clear_error(&err); g_free(debug); gst_message_unref(msg);
    }
    gst_object_unref(bus);
}

static void *play_thread_main(void *arg) {
    app_t *app = arg;
    int64_t due = mono_ms();
    int was_suppressed = 0;
    while (atomic_load(&app->running)) {
        int64_t now = mono_ms();
        if (now < due) { msleep_int((int)(due - now)); continue; }
        if (now - due >= PTT_FRAME_MS) {
            atomic_fetch_add(&app->playback_schedule_late, 1);
            due = now;
        }
        due += PTT_FRAME_MS;
        int16_t pcm[PTT_SAMPLES] = {0};
        int suppressed = atomic_load(&app->suppress_playback);
        unsigned suppressed_generation = atomic_load(&app->ptt_generation);
        pthread_mutex_lock(&app->jitter_lock);
        if (suppressed) ptt_jitter_suppress(&app->jitter);
        else ptt_jitter_render(&app->jitter, now, pcm);
        pthread_mutex_unlock(&app->jitter_lock);
        if (app->preamble_enabled && suppressed) {
            if (!was_suppressed) gst_element_set_state(app->playback_pipeline, GST_STATE_NULL);
            atomic_store(&app->playback_quiesced, suppressed_generation);
            was_suppressed = 1;
            continue;
        }
        if (app->preamble_enabled && was_suppressed && !suppressed) {
            if (atomic_load(&app->monitor_active)) continue;
            atomic_store(&app->playback_quiesced, 0);
            gst_element_set_state(app->playback_pipeline, GST_STATE_PLAYING);
        }
        if (suppressed && !was_suppressed) {
            /* Discard PCM already queued before pressing local PTT. */
            gst_element_set_state(app->playback_pipeline, GST_STATE_READY);
            gst_element_set_state(app->playback_pipeline, GST_STATE_PLAYING);
        }
        was_suppressed = suppressed;
        guint64 queued = 0;
        g_object_get(app->playback_src, "current-level-buffers", &queued, NULL);
        /* Count explicit drops, not a guess based on appsrc's internal leak
         * policy. The producer never waits for a stalled sound device. */
        if (queued >= 5) atomic_fetch_add(&app->playback_queue_drops, 1);
        GstBuffer *buffer = queued < 5 ? gst_buffer_new_allocate(NULL, sizeof(pcm), NULL) : NULL;
        if (buffer) {
            gst_buffer_fill(buffer, 0, pcm, sizeof(pcm));
            GST_BUFFER_DURATION(buffer) = PTT_FRAME_MS * GST_MSECOND;
            if (gst_app_src_push_buffer(GST_APP_SRC(app->playback_src), buffer) == GST_FLOW_OK)
                atomic_fetch_add(&app->rx_played_packets, 1);
            else atomic_fetch_add(&app->playback_errors, 1);
        }
        check_audio_bus(app, app->playback_pipeline);
    }
    return NULL;
}

static int file_exists_readable(const char *path) { return path && access(path, R_OK) == 0; }
static void init_ptt_tones(app_t *app) { snprintf(app->start_wav_path, sizeof(app->start_wav_path), "start.wav"); snprintf(app->stop_wav_path, sizeof(app->stop_wav_path), "stop.wav"); app->have_start_wav = file_exists_readable(app->start_wav_path); app->have_stop_wav = file_exists_readable(app->stop_wav_path); printf("ptt tones: start.wav=%s stop.wav=%s\n", app->have_start_wav ? "yes" : "no", app->have_stop_wav ? "yes" : "no"); fflush(stdout); }

static void *tone_thread_main(void *arg) {
    tone_play_req_t *req = (tone_play_req_t *)arg; if (!req) return NULL;
    GError *err = NULL; char desc[PATH_MAX + ALSA_DEV_MAX + 128];
    if (req->playback_device[0]) snprintf(desc, sizeof(desc), "filesrc location=\"%s\" ! wavparse ! audioconvert ! audioresample ! alsasink device=\"%s\" sync=false", req->wav_path, req->playback_device);
    else snprintf(desc, sizeof(desc), "filesrc location=\"%s\" ! wavparse ! audioconvert ! audioresample ! autoaudiosink", req->wav_path);
    GstElement *pipeline = gst_parse_launch(desc, &err);
    if (!pipeline || err) { fprintf(stderr, "tone playback pipeline error for %s: %s\n", req->wav_path, err ? err->message : "unknown"); if (err) g_error_free(err); if (pipeline) gst_object_unref(pipeline); free(req); return NULL; }
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(pipeline); if (bus) { gst_bus_timed_pop_filtered(bus, 2 * GST_SECOND, GST_MESSAGE_ERROR | GST_MESSAGE_EOS); gst_object_unref(bus); }
    gst_element_set_state(pipeline, GST_STATE_NULL); gst_object_unref(pipeline); free(req); return NULL;
}

static void play_tone_async(app_t *app, const char *path) {
    if (!app || !path || !path[0]) return;
    tone_play_req_t *req = calloc(1, sizeof(*req)); if (!req) return;
    snprintf(req->wav_path, sizeof(req->wav_path), "%s", path); snprintf(req->playback_device, sizeof(req->playback_device), "%s", app->alsa_playback_device);
    pthread_t tid; if (pthread_create(&tid, NULL, tone_thread_main, req) == 0) pthread_detach(tid); else free(req);
}

static void set_ptt_state(app_t *app, int pressed) {
    int old = atomic_exchange(&app->ptt_pressed, pressed); atomic_store(&app->suppress_playback, pressed);
    if (old != pressed) {
        atomic_store(&app->microphone_ready, 0);
        if (pressed) atomic_fetch_add(&app->ptt_generation, 1);
        printf("[%lld] PTT %s (txid=%s)\n", now_ms(), pressed ? (app->preamble_enabled ? "DOWN -> preamble; wait for cue to end" : "DOWN -> sending microphone audio") : "UP -> sending idle frames", app->txid); fflush(stdout);
        state_file_set_tx(app, pressed);
        if (pressed) { if (!app->preamble_enabled && app->have_start_wav) play_tone_async(app, app->start_wav_path); }
        else { if (app->have_stop_wav) play_tone_async(app, app->stop_wav_path); }
    }
}

static void update_effective_ptt_state(app_t *app) { int pressed = atomic_load(&app->ptt_keyboard_pressed) || atomic_load(&app->ptt_socket_pressed); set_ptt_state(app, pressed ? 1 : 0); }
static void set_keyboard_ptt_state(app_t *app, int pressed) { atomic_store(&app->ptt_keyboard_pressed, pressed ? 1 : 0); update_effective_ptt_state(app); }
static void set_socket_ptt_state(app_t *app, int pressed) { atomic_store(&app->ptt_socket_pressed, pressed ? 1 : 0); update_effective_ptt_state(app); }

static int make_ptt_control_socket(const char *path) {
    int fd; struct sockaddr_un addr; struct timeval tv;
    if (!path || !path[0]) return -1;
    if (strlen(path) >= sizeof(addr.sun_path)) { fprintf(stderr, "ptt socket path too long: %s\n", path); return -1; }
    unlink(path);
    fd = socket(AF_UNIX, SOCK_DGRAM, 0); if (fd < 0) { perror("socket(AF_UNIX)"); return -1; }
    memset(&addr, 0, sizeof(addr)); addr.sun_family = AF_UNIX; memcpy(addr.sun_path, path, strlen(path) + 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind(AF_UNIX)"); close(fd); unlink(path); return -1; }
    tv.tv_sec = 0; tv.tv_usec = 200000; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); return fd;
}

static void trim_ascii(char *s) {
    size_t len, start = 0, end; if (!s) return; len = strlen(s);
    while (start < len && isspace((unsigned char)s[start])) start++;
    if (start > 0) memmove(s, s + start, len - start + 1);
    len = strlen(s); end = len; while (end > 0 && isspace((unsigned char)s[end - 1])) end--; s[end] = '\0';
}

static void handle_ptt_control_command(app_t *app, char *cmd) {
    trim_ascii(cmd); if (cmd[0] == '\0') return;
    if (strcasecmp(cmd, "DOWN") == 0 || strcasecmp(cmd, "ON") == 0 || strcmp(cmd, "1") == 0) { printf("[%lld] PTT socket command: %s\n", now_ms(), cmd); fflush(stdout); set_socket_ptt_state(app, 1); }
    else if (strcasecmp(cmd, "UP") == 0 || strcasecmp(cmd, "OFF") == 0 || strcmp(cmd, "0") == 0) { printf("[%lld] PTT socket command: %s\n", now_ms(), cmd); fflush(stdout); set_socket_ptt_state(app, 0); }
    else if (strcasecmp(cmd, "TOGGLE") == 0) { int next = !atomic_load(&app->ptt_socket_pressed); printf("[%lld] PTT socket command: %s -> %s\n", now_ms(), cmd, next ? "DOWN" : "UP"); fflush(stdout); set_socket_ptt_state(app, next); }
    else { printf("[%lld] PTT socket unknown command: %s\n", now_ms(), cmd); fflush(stdout); }
}

static void *ptt_control_thread_main(void *arg) {
    app_t *app = (app_t *)arg; char buf[128];
    printf("ptt control: listening on unix socket %s\n", app->ptt_socket_path); fflush(stdout);
    while (atomic_load(&app->running)) {
        ssize_t n = recv(app->ptt_ctrl_sock, buf, sizeof(buf) - 1, 0);
        if (n < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue; perror("recv(AF_UNIX)"); break; }
        buf[n] = '\0'; handle_ptt_control_command(app, buf);
    }
    return NULL;
}

static void *keyboard_thread_main(void *arg) {
    app_t *app = (app_t *)arg; int fds[INPUT_SCAN_MAX]; int nfds = 0; int altgr_pending = 0; long long altgr_press_start_ms = 0;
    for (int i = 0; i < INPUT_SCAN_MAX; ++i) { char path[64]; snprintf(path, sizeof(path), "/dev/input/event%d", i); int fd = open(path, O_RDONLY | O_NONBLOCK); if (fd >= 0) fds[nfds++] = fd; }
    if (nfds == 0) { fprintf(stderr, "keyboard debug: no /dev/input/event* readable\n"); return NULL; }
    if (app->codec_ptt_enabled) printf("keyboard debug: monitoring codec PTT key (KEY_ENTER) on %d input device(s)\n", nfds);
    else printf("keyboard debug: monitoring Right Alt / AltGr on %d input device(s), hold threshold=%d ms\n", nfds, app->pc_ptt_hold_ms);
    fflush(stdout);

    while (atomic_load(&app->running)) {
        bool had_event = false;
        for (int i = 0; i < nfds; ++i) {
            struct input_event ev; ssize_t n;
            while ((n = read(fds[i], &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
                had_event = true; if (ev.type != EV_KEY) continue;
                if (app->codec_ptt_enabled) { if (ev.code == KEY_ENTER) set_keyboard_ptt_state(app, ev.value != 0); continue; }
                if (ev.code == KEY_RIGHTALT) {
                    if (ev.value != 0) { if (!altgr_pending && !atomic_load(&app->ptt_keyboard_pressed)) { altgr_pending = 1; altgr_press_start_ms = now_ms(); } }
                    else { altgr_pending = 0; if (atomic_load(&app->ptt_keyboard_pressed)) set_keyboard_ptt_state(app, 0); }
                }
            }
        }
        if (!app->codec_ptt_enabled && altgr_pending && !atomic_load(&app->ptt_keyboard_pressed)) {
            long long held_ms = now_ms() - altgr_press_start_ms; if (held_ms >= app->pc_ptt_hold_ms) { altgr_pending = 0; set_keyboard_ptt_state(app, 1); }
        }
        if (!had_event) msleep_int(10);
    }
    for (int i = 0; i < nfds; ++i) {
        close(fds[i]);
    }
    return NULL;
}

static void finish_tx_session(app_t *app) {
    /* Repetition is one-way compatible; receivers treat END idempotently. */
    for (int i = 0; i < 3; ++i) send_packet(app, PKT_END, NULL, 0);
}

static void *send_preamble_thread(void *arg);

static void *send_thread_main(void *arg) {
    if (((app_t *)arg)->preamble_enabled) return send_preamble_thread(arg);
    app_t *app = arg;
    unsigned generation = 0;
    int transmitting = 0;
    GstClockTime last_pts = GST_CLOCK_TIME_NONE;
    while (atomic_load(&app->running)) {
        unsigned current = atomic_load(&app->ptt_generation);
        int pressed = app->ptt_enabled && atomic_load(&app->ptt_pressed);
        if (transmitting && (!pressed || current != generation)) {
            finish_tx_session(app); transmitting = 0;
        }
        if (pressed && !transmitting) {
            /* Capture runs while idle: discard old microphone frames. */
            GstSample *stale;
            while ((stale = gst_app_sink_try_pull_sample(GST_APP_SINK(app->capture_sink), 0)))
                gst_sample_unref(stale);
            randombytes_buf(app->tx_session, sizeof(app->tx_session));
            app->tx_seq = 0;
            last_pts = GST_CLOCK_TIME_NONE;
            generation = current;
            transmitting = 1;
        }
        if (transmitting) {
            GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(app->capture_sink), 50 * GST_MSECOND);
            if (sample) {
                GstBuffer *buffer = gst_sample_get_buffer(sample); GstMapInfo map;
                if (atomic_load(&app->ptt_pressed) && generation == atomic_load(&app->ptt_generation) &&
                    gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                    GstClockTime pts = GST_BUFFER_PTS(buffer);
                    if (GST_CLOCK_TIME_IS_VALID(pts) && GST_CLOCK_TIME_IS_VALID(last_pts) && pts > last_pts) {
                        uint64_t frames = (pts - last_pts + 10 * GST_MSECOND) / (PTT_FRAME_MS * GST_MSECOND);
                        if (frames > 1 && frames < UINT32_MAX) {
                            app->tx_seq += (uint32_t)(frames - 1);
                            atomic_fetch_add(&app->tx_capture_gaps, (unsigned long)(frames - 1));
                        }
                    }
                    last_pts = pts;
                    if (map.size <= PTT_OPUS_MAX &&
                        opus_packet_get_nb_samples(map.data, (opus_int32)map.size, 48000) == PTT_SAMPLES &&
                        send_packet(app, PKT_AUDIO, map.data, (uint16_t)map.size))
                        atomic_fetch_add(&app->tx_audio_packets, 1);
                    gst_buffer_unmap(buffer, &map);
                }
                gst_sample_unref(sample);
            }
        } else {
            if (send_packet(app, PKT_IDLE, NULL, 0)) atomic_fetch_add(&app->tx_idle_packets, 1);
            msleep_int(20);
        }
    }
    if (transmitting) finish_tx_session(app);
    return NULL;
}

/* Capture PCM only in preamble mode: one libopus encoder handles both sources. */
static GstElement *make_raw_capture_pipeline(GstElement **sink, const char *device) {
    char desc[768]; GError *error = NULL;
    if (device[0]) snprintf(desc, sizeof(desc), "alsasrc device=\"%s\" ! audioconvert ! audioresample ! audio/x-raw,format=%s,channels=1,rate=48000,layout=interleaved ! appsink name=raw sync=false max-buffers=8 drop=true", device, GST_AUDIO_NE(S16));
    else snprintf(desc, sizeof(desc), "autoaudiosrc ! audioconvert ! audioresample ! audio/x-raw,format=%s,channels=1,rate=48000,layout=interleaved ! appsink name=raw sync=false max-buffers=8 drop=true", GST_AUDIO_NE(S16));
    GstElement *pipeline = gst_parse_launch(desc, &error);
    if (!pipeline || error) {
        fprintf(stderr, "raw capture: %s\n", error ? error->message : "creation failed");
        if (error) g_error_free(error);
        if (pipeline) gst_object_unref(pipeline);
        return NULL;
    }
    *sink = gst_bin_get_by_name(GST_BIN(pipeline), "raw");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    return pipeline;
}

/* A finite, clocked stream. EOS is delivered by the sink after the cue has
 * played, rather than when appsrc has merely accepted its final buffer. */
static GstElement *make_monitor(app_t *app, GstElement **source) {
    char desc[768]; GError *error = NULL;
    const char *prefix = "appsrc name=cue format=time is-live=false block=false max-bytes=0 ! audioconvert ! audioresample ! ";
    if (app->alsa_playback_device[0]) snprintf(desc, sizeof(desc), "%salsasink device=\"%s\" sync=true", prefix, app->alsa_playback_device);
    else snprintf(desc, sizeof(desc), "%sautoaudiosink sync=true", prefix);
    GstElement *pipeline = gst_parse_launch(desc, &error);
    if (!pipeline || error) {
        fprintf(stderr, "preamble monitor: %s\n", error ? error->message : "creation failed");
        if (error) g_error_free(error);
        if (pipeline) gst_object_unref(pipeline);
        return NULL;
    }
    *source = gst_bin_get_by_name(GST_BIN(pipeline), "cue");
    GstCaps *caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, GST_AUDIO_NE(S16), "layout", G_TYPE_STRING, "interleaved", "rate", G_TYPE_INT, 48000, "channels", G_TYPE_INT, 1, NULL);
    g_object_set(*source, "caps", caps, NULL); gst_caps_unref(caps);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    return pipeline;
}

static void read_fix(app_t *app, tm_record *record) {
    record->gps = 0;
    if (!app->gps_file[0]) return;
    /* Never block on a FIFO/device supplied accidentally as a fix file. */
    int fd = open(app->gps_file, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > 256) { close(fd); return; }
    char data[257]; ssize_t n = read(fd, data, 256); close(fd);
    if (n <= 0) return;
    data[n] = 0;
    double lat, lon, timestamp; char extra;
    if (sscanf(data, "%lf %lf %lf %c", &lat, &lon, &timestamp, &extra) != 3 ||
        !isfinite(lat) || !isfinite(lon) || !isfinite(timestamp) ||
        lat < -90 || lat > 90 || lon < -180 || lon > 180) return;
    double age = now_ms() / 1000. - timestamp;
    if (age < 0 || age > 30) return;
    record->gps = 1; record->latitude = (int32_t)llround(lat * 100000);
    record->longitude = (int32_t)llround(lon * 100000); record->age = (uint16_t)ceil(age);
}

static void drain_capture(app_t *app, GstAdapter *adapter) {
    gst_adapter_clear(adapter);
    /* Bounded even for a defective source that produces without pacing. */
    for (int i = 0; i < 16; i++) {
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(app->capture_sink), 0);
        if (!sample) break;
        gst_sample_unref(sample);
    }
}

static void *send_preamble_thread(void *arg) {
    app_t *app = arg;
    int error; OpusEncoder *encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error);
    if (!encoder) { fprintf(stderr, "preamble encoder: %s\n", opus_strerror(error)); atomic_store(&app->running, 0); return NULL; }
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(app->fec_enabled));
    opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(app->loss_percent));
    GstAdapter *adapter = gst_adapter_new();
    GstElement *monitor = NULL, *source = NULL; GstBus *bus = NULL;
    tm_burst burst; unsigned frame = 0, generation = 0;
    int active = 0, failed = 0, eos = 0;
    int64_t due = mono_ms(), monitor_deadline = 0;
    GstClockTime capture_end = GST_CLOCK_TIME_NONE;
    while (atomic_load(&app->running)) {
        int pressed = atomic_load(&app->ptt_pressed);
        unsigned current = atomic_load(&app->ptt_generation);
        if (active && (!pressed || current != generation)) {
            if (monitor) { gst_element_set_state(monitor, GST_STATE_NULL); gst_object_unref(source); gst_object_unref(bus); gst_object_unref(monitor); monitor = source = NULL; bus = NULL; atomic_store(&app->monitor_active, 0); }
            finish_tx_session(app); active = 0; atomic_store(&app->microphone_ready, 0);
        }
        if (!pressed) { failed = 0; send_packet(app, PKT_IDLE, NULL, 0); atomic_fetch_add(&app->tx_idle_packets, 1); msleep_int(20); continue; }
        if (failed && current == generation) { msleep_int(10); continue; }
        if (!active) {
            /* The playback owner releases the sound device before opening the cue. */
            if (app->playback_pipeline && atomic_load(&app->playback_quiesced) != current) { msleep_int(5); continue; }
            tm_record record = app->telemetry_tx; read_fix(app, &record);
            record.sequence = app->telemetry_tx.sequence++;
            if (!tm_build(&burst, &record)) { fprintf(stderr, "invalid telemetry record\n"); atomic_store(&app->running, 0); break; }
            generation = current; failed = 0;
            atomic_store(&app->monitor_active, 1);
            monitor = make_monitor(app, &source);
            if (!monitor) { atomic_store(&app->monitor_active, 0); failed = 1; continue; }
            bus = gst_element_get_bus(monitor);
            randombytes_buf(app->tx_session, sizeof(app->tx_session)); app->tx_seq = 0;
            opus_encoder_ctl(encoder, OPUS_RESET_STATE);
            drain_capture(app, adapter); capture_end = GST_CLOCK_TIME_NONE;
            active = 1; frame = 0; eos = 0; due = mono_ms();
            monitor_deadline = due + burst.frames * 20 + 3000;
        }
        int64_t now = mono_ms();
        if (now < due) { msleep_int((int)(due - now > 5 ? 5 : due - now)); continue; }
        if (now - due >= 20) due = now;
        due += 20;
        int16_t pcm[PTT_SAMPLES] = {0};
        int is_ready = atomic_load(&app->microphone_ready);
        if (!is_ready) {
            if (!eos) drain_capture(app, adapter);
            if (frame < burst.frames) {
                tm_frame(&burst, frame, pcm);
                /* Monitor the audible portion only. The final on-air silence
                 * lets capture retain speech beginning immediately after EOS. */
                GstBuffer *buffer = frame < burst.frames - 5 ? gst_buffer_new_allocate(NULL, sizeof(pcm), NULL) : NULL;
                if (!buffer && frame < burst.frames - 5) { failed = 1; }
                else if (buffer) {
                    gst_buffer_fill(buffer, 0, pcm, sizeof(pcm));
                    GST_BUFFER_PTS(buffer) = frame * 20 * GST_MSECOND;
                    GST_BUFFER_DURATION(buffer) = 20 * GST_MSECOND;
                    if (gst_app_src_push_buffer(GST_APP_SRC(source), buffer) != GST_FLOW_OK) failed = 1;
                }
                frame++;
                if (frame == burst.frames - 5 && gst_app_src_end_of_stream(GST_APP_SRC(source)) != GST_FLOW_OK) failed = 1;
            }
            GstMessage *message;
            while ((message = gst_bus_pop_filtered(bus, GST_MESSAGE_EOS | GST_MESSAGE_ERROR))) {
                if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) failed = 1;
                else eos = 1;
                gst_message_unref(message);
            }
            if (!eos && now >= monitor_deadline) failed = 1;
            if (failed) {
                fprintf(stderr, "preamble monitor failed or timed out; release PTT and retry\n");
                gst_element_set_state(monitor, GST_STATE_NULL); gst_object_unref(source); gst_object_unref(bus); gst_object_unref(monitor); monitor = source = NULL; bus = NULL; atomic_store(&app->monitor_active, 0);
                finish_tx_session(app); active = 0; continue;
            }
            if (eos && frame == burst.frames && atomic_load(&app->ptt_pressed) && generation == atomic_load(&app->ptt_generation)) {
                gst_element_set_state(monitor, GST_STATE_NULL); gst_object_unref(source); gst_object_unref(bus); gst_object_unref(monitor); monitor = source = NULL; bus = NULL; atomic_store(&app->monitor_active, 0);
                capture_end = GST_CLOCK_TIME_NONE;
                atomic_store(&app->microphone_ready, 1);
                printf("[%lld] PTT READY id=%s (microphone enabled)\n", now_ms(), app->telemetry_tx.id);
            }
        } else {
            /* Accumulate arbitrary capture buffer sizes into 20 ms PCM frames. */
            while (gst_adapter_available(adapter) < sizeof(pcm)) {
                GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(app->capture_sink), 0);
                if (!sample) break;
                GstBuffer *buffer = gst_sample_get_buffer(sample);
                GstClockTime pts = GST_BUFFER_PTS(buffer);
                if (GST_CLOCK_TIME_IS_VALID(pts) && GST_CLOCK_TIME_IS_VALID(capture_end) && pts > capture_end + 10 * GST_MSECOND) {
                    gst_adapter_clear(adapter);
                    atomic_fetch_add(&app->tx_capture_gaps, 1);
                }
                capture_end = GST_CLOCK_TIME_IS_VALID(pts) ? pts + gst_util_uint64_scale(gst_buffer_get_size(buffer) / 2, GST_SECOND, 48000) : GST_CLOCK_TIME_NONE;
                gst_adapter_push(adapter, gst_buffer_ref(buffer)); gst_sample_unref(sample);
            }
            if (gst_adapter_available(adapter) >= sizeof(pcm)) {
                gst_adapter_copy(adapter, pcm, 0, sizeof(pcm)); gst_adapter_flush(adapter, sizeof(pcm));
            }
        }
        if (!atomic_load(&app->ptt_pressed) || generation != atomic_load(&app->ptt_generation)) continue;
        unsigned char packet[PTT_OPUS_MAX]; int n = opus_encode(encoder, pcm, PTT_SAMPLES, packet, sizeof(packet));
        if (n > 0 && send_packet(app, PKT_AUDIO, packet, (uint16_t)n)) atomic_fetch_add(&app->tx_audio_packets, 1);
    }
    if (monitor) { gst_element_set_state(monitor, GST_STATE_NULL); gst_object_unref(source); gst_object_unref(bus); gst_object_unref(monitor); }
    atomic_store(&app->monitor_active, 0);
    if (active) finish_tx_session(app);
    atomic_store(&app->microphone_ready, 0);
    g_object_unref(adapter); opus_encoder_destroy(encoder); return NULL;
}

static void output_telemetry(app_t *app) {
    for (;;) {
        tm_record r;
        pthread_mutex_lock(&app->jitter_lock);
        int have = app->jitter.event_read != app->jitter.event_write;
        if (have) r = app->jitter.events[app->jitter.event_read++ % 16];
        pthread_mutex_unlock(&app->jitter_lock);
        if (!have) break;
        char gps[160] = "", json[512];
        if (r.gps) snprintf(gps, sizeof(gps), ",\"latitude\":%.5f,\"longitude\":%.5f,\"fix_age_s\":%u", r.latitude / 100000., r.longitude / 100000., r.age);
        snprintf(json, sizeof(json), "{\"type\":\"udpptt.telemetry\",\"version\":1,\"received_at_ms\":%lld,\"id\":\"%s\",\"sequence\":%u,\"gps\":%s%s}", now_ms(), r.id, r.sequence, r.gps ? "true" : "false", gps);
        printf("TELEMETRY %s\n", json); fflush(stdout);
        if (app->telemetry_fd >= 0 && app->telemetry_socket[0]) {
            struct sockaddr_un address = {.sun_family = AF_UNIX};
            snprintf(address.sun_path, sizeof(address.sun_path), "%s", app->telemetry_socket);
            if (sendto(app->telemetry_fd, json, strlen(json), MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)&address, sizeof(address)) < 0)
                fprintf(stderr, "telemetry event delivery: %s\n", strerror(errno));
        }
    }
}

static ssize_t recv_transport_packet(app_t *app, uint8_t *buf, size_t buf_sz) {
    if (app->transport_mode == TRANSPORT_UDP) {
        return recv(app->sock, buf, buf_sz, 0);
    } else {
        uint8_t frame[MAX_FRAME];
        ssize_t n = recv(app->sock, frame, sizeof(frame), 0);
        if (n <= 0) return n;
        if ((size_t)n < sizeof(bf_eth_hdr_t)) return -2;
        bf_eth_hdr_t hdr;
        memcpy(&hdr, frame, sizeof(hdr));
        if (ntohs(hdr.ethertype) != app->bf_ethertype) return -2;
        if (memcmp(hdr.src, app->bf_local_mac, ETH_ALEN) == 0) return -2;
        size_t payload_len = (size_t)n - sizeof(bf_eth_hdr_t);
        if (payload_len > buf_sz) payload_len = buf_sz;
        memcpy(buf, frame + sizeof(bf_eth_hdr_t), payload_len);
        return (ssize_t)payload_len;
    }
}

static void *recv_thread_main(void *arg) {
    app_t *app = arg;
    uint8_t buf[MAX_PACKET];
    while (atomic_load(&app->running)) {
        ssize_t n = recv_transport_packet(app, buf, sizeof(buf));
        if (n < 0) {
            if (n == -2 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                state_file_check_rx_timeout(app); continue;
            }
            perror("recv"); break;
        }
        packet_hdr_t hdr;
        if ((size_t)n < sizeof(hdr)) { atomic_fetch_add(&app->rx_invalid_packets, 1); continue; }
        memcpy(&hdr, buf, sizeof(hdr));
        if (!ptt_header_valid(&hdr, (size_t)n)) {
            unsigned long bad = atomic_fetch_add(&app->rx_invalid_packets, 1) + 1;
            if (bad == 1 || bad % 100 == 0) fprintf(stderr, "RX invalid/unsupported packet (protocol v2 required), count=%lu\n", bad);
            continue;
        }
        unsigned char plain[MAX_PACKET];
        const uint8_t *opus = buf + sizeof(hdr);
        unsigned long long len = ntohs(hdr.len);
        if (!!(hdr.flags & PKT_FLAG_ENCRYPTED) != !!app->encrypt_enabled) {
            atomic_fetch_add(&app->rx_decrypt_failures, 1); continue;
        }
        if (app->encrypt_enabled) {
            if (crypto_aead_xchacha20poly1305_ietf_decrypt(plain, &len, NULL,
                opus, len, (const unsigned char *)&hdr, sizeof(hdr), hdr.nonce, app->key) != 0) {
                atomic_fetch_add(&app->rx_decrypt_failures, 1); continue;
            }
            opus = plain;
        }
        if (hdr.type == PKT_IDLE) {
            atomic_fetch_add(&app->rx_ignored_idle_packets, 1);
            state_file_check_rx_timeout(app); continue;
        }
        if (hdr.type == PKT_AUDIO) {
            atomic_fetch_add(&app->rx_audio_packets, 1);
            state_file_note_rx_audio(app, hdr.talk_id);
        }
        pthread_mutex_lock(&app->jitter_lock);
        /* Register and then retire the session while muted, so its delayed
         * packets cannot play after local PTT is released. */
        ptt_jitter_put(&app->jitter, &hdr, opus, (size_t)len, mono_ms());
        if (atomic_load(&app->suppress_playback)) {
            ptt_jitter_suppress(&app->jitter);
            atomic_fetch_add(&app->rx_suppressed_packets, 1);
        }
        pthread_mutex_unlock(&app->jitter_lock);
        state_file_check_rx_timeout(app);
    }
    return NULL;
}

static void cleanup(app_t *app) {
    atomic_store(&app->running, 0);
    if (app->ptt_ctrl_sock >= 0) { close(app->ptt_ctrl_sock); app->ptt_ctrl_sock = -1; }
    if (app->key_thread) pthread_join(app->key_thread, NULL);
    if (app->ctrl_thread) pthread_join(app->ctrl_thread, NULL);
    if (app->send_thread) pthread_join(app->send_thread, NULL);
    if (app->recv_thread) pthread_join(app->recv_thread, NULL);
    if (app->play_thread) pthread_join(app->play_thread, NULL);
    report_rx_stats(app);
    ptt_jitter_destroy(&app->jitter);
    if (app->capture_pipeline) { gst_element_set_state(app->capture_pipeline, GST_STATE_NULL); if (app->capture_sink) gst_object_unref(app->capture_sink); gst_object_unref(app->capture_pipeline); }
    if (app->playback_pipeline) { gst_element_set_state(app->playback_pipeline, GST_STATE_NULL); if (app->playback_src) { gst_app_src_end_of_stream(GST_APP_SRC(app->playback_src)); gst_object_unref(app->playback_src); } gst_object_unref(app->playback_pipeline); }
    if (app->telemetry_fd >= 0) { close(app->telemetry_fd); app->telemetry_fd = -1; }
    if (app->sock >= 0) { close(app->sock); app->sock = -1; }
    if (app->state_file_enabled) state_file_write_idle(app);
    if (app->ptt_socket_enabled && app->ptt_socket_path[0]) unlink(app->ptt_socket_path);
    sodium_memzero(app->key, sizeof(app->key));
    printf("summary: tx_audio=%lu tx_idle=%lu rx_audio=%lu pcm_blocks_queued=%lu rx_idle_ignored=%lu decrypt_fail=%lu\n", (unsigned long)atomic_load(&app->tx_audio_packets), (unsigned long)atomic_load(&app->tx_idle_packets), (unsigned long)atomic_load(&app->rx_audio_packets), (unsigned long)atomic_load(&app->rx_played_packets), (unsigned long)atomic_load(&app->rx_ignored_idle_packets), (unsigned long)atomic_load(&app->rx_decrypt_failures));
    pthread_mutex_destroy(&app->state_lock);
    pthread_mutex_destroy(&app->jitter_lock);
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s <server-ip> [--port PORT] [--txid NAME] [--rx-only|--no-ptt] [--encrypt] [--key PASSWORD]\n"
            "       [--codec-ptt] [--ptt-socket PATH] [--state-file PATH] [--rx-state-timeout-ms MS]\n"
            "       [--altgr-ptt-delay-ms MS] [--rpi-audio]\n"
            "       [--alsa-device DEV] [--alsa-capture-device DEV] [--alsa-playback-device DEV]\n"
            "       [--preamble-id ABCDE] [--gps-file PATH] [--telemetry-socket PATH]\n"
            "       [--jitter-ms 0..1000] [--fec-loss-percent 0..100] [--no-fec]\n"
            "       [--blackfiber IFACE] [--bf-dst-mac MAC] [--bf-ethertype ETHERTYPE]\n"
            "       [--blackfiber-rx-passive] [--blackfiber-tx-only]\n", argv0);
}

int main(int argc, char **argv) {
    memset(&g_app, 0, sizeof(g_app)); pthread_mutex_init(&g_app.state_lock, NULL); pthread_mutex_init(&g_app.jitter_lock, NULL); g_app.jitter_ms = -1; g_app.loss_percent = 10; g_app.fec_enabled = 1; g_app.telemetry_fd = -1; g_app.sock = -1; g_app.ptt_ctrl_sock = -1; g_app.server_port = DEFAULT_SERVER_PORT; g_app.transport_mode = TRANSPORT_UDP; g_app.ptt_enabled = 1; g_app.pc_ptt_hold_ms = 2000; g_app.rx_state_timeout_ms = 1000; g_app.bf_ethertype = DEFAULT_BLACKFIBER_ETHERTYPE; memset(g_app.bf_dst_mac, 0xff, sizeof(g_app.bf_dst_mac)); snprintf(g_app.txid, sizeof(g_app.txid), "anon");
    const char *server_ip = NULL; const char *password = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--rx-only") || !strcmp(argv[i], "--no-ptt")) g_app.ptt_enabled = 0;
        else if (!strcmp(argv[i], "--preamble-id")) {
            if (i + 1 >= argc || !tm_id(g_app.telemetry_tx.id, argv[++i])) {
                fprintf(stderr, "--preamble-id requires exactly five letters A-Z\n"); return 1;
            }
            g_app.preamble_enabled = 1;
        }
        else if (!strcmp(argv[i], "--gps-file")) {
            if (i + 1 >= argc || !argv[i+1][0] || strlen(argv[i+1]) >= sizeof(g_app.gps_file)) return 1;
            copy_opt_string(g_app.gps_file, sizeof(g_app.gps_file), argv[++i]);
        }
        else if (!strcmp(argv[i], "--telemetry-socket")) {
            if (i + 1 >= argc || !argv[i+1][0] || strlen(argv[i+1]) >= sizeof(g_app.telemetry_socket)) return 1;
            copy_opt_string(g_app.telemetry_socket, sizeof(g_app.telemetry_socket), argv[++i]);
        }
        else if (!strcmp(argv[i], "--no-fec")) g_app.fec_enabled = 0;
        else if (!strcmp(argv[i], "--jitter-ms") || !strcmp(argv[i], "--fec-loss-percent")) {
            int jitter = !strcmp(argv[i], "--jitter-ms");
            if (i + 1 >= argc) { fprintf(stderr, "missing option value\n"); return 1; }
            char *end; errno = 0; long v = strtol(argv[++i], &end, 10);
            if (errno || !argv[i][0] || *end || v < 0 || v > (jitter ? 1000 : 100)) {
                fprintf(stderr, "invalid recovery option value: %s\n", argv[i]); return 1;
            }
            if (jitter) g_app.jitter_ms = (int)v; else g_app.loss_percent = (int)v;
        }
        else if (!strcmp(argv[i], "--encrypt")) g_app.encrypt_enabled = 1;
        else if (!strcmp(argv[i], "--codec-ptt")) g_app.codec_ptt_enabled = 1;
        else if (!strcmp(argv[i], "--ptt-socket") || !strcmp(argv[i], "--control-socket")) { if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", argv[i]); return 1; } copy_opt_string(g_app.ptt_socket_path, sizeof(g_app.ptt_socket_path), argv[++i]); g_app.ptt_socket_enabled = 1; }
        else if (!strcmp(argv[i], "--state-file") || !strcmp(argv[i], "--ptt-state-file")) { if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", argv[i]); return 1; } copy_opt_string(g_app.state_file_path, sizeof(g_app.state_file_path), argv[++i]); g_app.state_file_enabled = 1; }
        else if (!strcmp(argv[i], "--rx-state-timeout-ms") || !strcmp(argv[i], "--rx-indicator-timeout-ms")) { if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", argv[i]); return 1; } char *endp = NULL; long v = strtol(argv[++i], &endp, 10); if (!endp || *endp != '\0' || v < 0 || v > 60000) { fprintf(stderr, "invalid value for --rx-state-timeout-ms: %s\n", argv[i]); return 1; } g_app.rx_state_timeout_ms = (int)v; }
        else if (!strcmp(argv[i], "--blackfiber")) { if (i + 1 >= argc) { fprintf(stderr, "missing value for --blackfiber\n"); return 1; } g_app.transport_mode = TRANSPORT_BLACKFIBER; copy_opt_string(g_app.bf_ifname, sizeof(g_app.bf_ifname), argv[++i]); }
        else if (!strcmp(argv[i], "--blackfiber-rx-passive")) { g_app.blackfiber_rx_passive = 1; }
        else if (!strcmp(argv[i], "--blackfiber-tx-only")) { g_app.blackfiber_tx_only = 1; }
        else if (!strcmp(argv[i], "--bf-dst-mac")) { if (i + 1 >= argc) { fprintf(stderr, "missing value for --bf-dst-mac\n"); return 1; } if (parse_mac_address(argv[++i], g_app.bf_dst_mac) != 0) { fprintf(stderr, "invalid MAC address: %s\n", argv[i]); return 1; } }
        else if (!strcmp(argv[i], "--bf-ethertype")) { if (i + 1 >= argc) { fprintf(stderr, "missing value for --bf-ethertype\n"); return 1; } if (parse_ethertype(argv[++i], &g_app.bf_ethertype) != 0) { fprintf(stderr, "invalid ethertype: %s\n", argv[i]); return 1; } }
        else if (!strcmp(argv[i], "--port") || !strcmp(argv[i], "--udp-port")) { int p; if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", argv[i]); return 1; } p = parse_udp_port(argv[++i]); if (p < 0) { fprintf(stderr, "invalid UDP port: %s\n", argv[i]); return 1; } g_app.server_port = p; }
        else if (!strcmp(argv[i], "--altgr-ptt-delay-ms")) { if (i + 1 >= argc) { fprintf(stderr, "missing value for --altgr-ptt-delay-ms\n"); return 1; } char *endp = NULL; long v = strtol(argv[++i], &endp, 10); if (!endp || *endp != '\0' || v < 0 || v > 60000) { fprintf(stderr, "invalid value for --altgr-ptt-delay-ms: %s\n", argv[i]); return 1; } g_app.pc_ptt_hold_ms = (int)v; }
        else if (!strcmp(argv[i], "--txid")) { if (i + 1 >= argc) { fprintf(stderr, "missing value for --txid\n"); return 1; } sanitize_txid(g_app.txid, sizeof(g_app.txid), argv[++i]); }
        else if (!strcmp(argv[i], "--key")) { if (i + 1 >= argc) { fprintf(stderr, "missing value for --key\n"); return 1; } password = argv[++i]; }
        else if (!strcmp(argv[i], "--alsa-device")) { if (i + 1 >= argc) { fprintf(stderr, "missing value for --alsa-device\n"); return 1; } copy_opt_string(g_app.alsa_capture_device, sizeof(g_app.alsa_capture_device), argv[++i]); copy_opt_string(g_app.alsa_playback_device, sizeof(g_app.alsa_playback_device), g_app.alsa_capture_device); }
        else if (!strcmp(argv[i], "--alsa-capture-device")) { if (i + 1 >= argc) { fprintf(stderr, "missing value for --alsa-capture-device\n"); return 1; } copy_opt_string(g_app.alsa_capture_device, sizeof(g_app.alsa_capture_device), argv[++i]); }
        else if (!strcmp(argv[i], "--alsa-playback-device")) { if (i + 1 >= argc) { fprintf(stderr, "missing value for --alsa-playback-device\n"); return 1; } copy_opt_string(g_app.alsa_playback_device, sizeof(g_app.alsa_playback_device), argv[++i]); }
        else if (!strcmp(argv[i], "--rpi-audio")) { copy_opt_string(g_app.alsa_capture_device, sizeof(g_app.alsa_capture_device), "plughw:0,0"); copy_opt_string(g_app.alsa_playback_device, sizeof(g_app.alsa_playback_device), "plughw:0,0"); }
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option: %s\n", argv[i]); usage(argv[0]); return 1; }
        else if (!server_ip) server_ip = argv[i];
        else { fprintf(stderr, "unexpected extra argument: %s\n", argv[i]); return 1; }
    }

    if (g_app.transport_mode == TRANSPORT_UDP) {
        if (g_app.blackfiber_tx_only || g_app.blackfiber_rx_passive) {
            fprintf(stderr, "Blackfiber direction options require --blackfiber IFACE\n"); return 1;
        }
        if (!server_ip) { usage(argv[0]); return 1; }
    } else {
        if (!g_app.bf_ifname[0]) { fprintf(stderr, "--blackfiber requires interface name\n"); return 1; }
        if (g_app.blackfiber_rx_passive && g_app.blackfiber_tx_only) {
            fprintf(stderr, "--blackfiber-rx-passive and --blackfiber-tx-only cannot be used together\n");
            return 1;
        }
        if (g_app.blackfiber_rx_passive) {
            g_app.ptt_enabled = 0;
        }
    }

    if (g_app.gps_file[0] && !g_app.preamble_enabled) { fprintf(stderr, "--gps-file requires --preamble-id\n"); return 1; }
    if (g_app.telemetry_socket[0]) {
        g_app.telemetry_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (g_app.telemetry_fd < 0) { perror("telemetry socket"); return 1; }
    }
    if (g_app.jitter_ms < 0) g_app.jitter_ms = g_app.transport_mode == TRANSPORT_UDP ? 120 : 40;
    ptt_jitter_init(&g_app.jitter, g_app.jitter_ms, g_app.fec_enabled);
    if (g_app.ptt_socket_enabled && !g_app.ptt_socket_path[0]) { fprintf(stderr, "--ptt-socket requires a non-empty path\n"); return 1; }
    if (g_app.state_file_enabled && !g_app.state_file_path[0]) { fprintf(stderr, "--state-file requires a non-empty path\n"); return 1; }
    if (sodium_init() < 0) { fprintf(stderr, "libsodium init failed\n"); return 1; }
    if (g_app.encrypt_enabled) { if (!password || !password[0]) password = getenv("UDPPTT_KEY"); if (!password || !password[0]) { fprintf(stderr, "--encrypt requires --key PASSWORD or UDPPTT_KEY\n"); return 1; } if (derive_key_from_password(password, g_app.key) != 0) return 1; }

    signal(SIGINT, on_sigint); signal(SIGTERM, on_sigint); gst_init(&argc, &argv);

    if (g_app.transport_mode == TRANSPORT_UDP) {
        g_app.sock = make_udp_socket(server_ip, g_app.server_port, &g_app.server_addr); if (g_app.sock < 0) return 1;
    } else {
        g_app.sock = make_blackfiber_socket(&g_app); if (g_app.sock < 0) return 1;
    }

    if (g_app.ptt_socket_enabled) { g_app.ptt_ctrl_sock = make_ptt_control_socket(g_app.ptt_socket_path); if (g_app.ptt_ctrl_sock < 0) { cleanup(&g_app); return 1; } }
    if (g_app.ptt_enabled) { g_app.capture_pipeline = g_app.preamble_enabled ? make_raw_capture_pipeline(&g_app.capture_sink, g_app.alsa_capture_device) : make_capture_pipeline(&g_app.capture_sink, g_app.alsa_capture_device, g_app.fec_enabled, g_app.loss_percent); if (!g_app.capture_pipeline) { cleanup(&g_app); return 1; } }
    if (!g_app.blackfiber_tx_only) {
        g_app.playback_pipeline = make_playback_pipeline(&g_app.playback_src, g_app.alsa_playback_device);
        if (!g_app.playback_pipeline) { cleanup(&g_app); return 1; }
    }
    init_ptt_tones(&g_app);
    atomic_store(&g_app.running, 1); atomic_store(&g_app.ptt_pressed, 0); atomic_store(&g_app.ptt_keyboard_pressed, 0); atomic_store(&g_app.ptt_socket_pressed, 0); atomic_store(&g_app.suppress_playback, 0);
    if (g_app.state_file_enabled) state_file_write_idle(&g_app);

    if (g_app.transport_mode == TRANSPORT_UDP) {
        printf("connected to %s:%d\n", server_ip, g_app.server_port);
    } else {
        char mac_local[32], mac_dst[32];
        format_mac(mac_local, sizeof(mac_local), g_app.bf_local_mac);
        format_mac(mac_dst, sizeof(mac_dst), g_app.bf_dst_mac);
        printf("blackfiber: iface=%s ethertype=0x%04x local-mac=%s dst-mac=%s\n", g_app.bf_ifname, g_app.bf_ethertype, mac_local, mac_dst);
        if (g_app.blackfiber_rx_passive) {
            printf("blackfiber mode: rx-passive (no transmitted idle/audio frames)\n");
        }
        if (g_app.blackfiber_tx_only) {
            printf("blackfiber mode: tx-only (receive thread disabled)\n");
        }
    }

    printf("protocol=v2 jitter_ms=%d fec=%s expected_loss=%d%%\n", g_app.jitter_ms, g_app.fec_enabled ? "on" : "off", g_app.loss_percent);
    printf("txid: %s\n", g_app.txid); printf("debug: shows PTT state, transmitted mic packets, received audio packets, and playback events\n");
    if (!g_app.ptt_enabled) printf("mode: receive-only (--rx-only), keyboard PTT disabled, microphone capture disabled\n");
    if (g_app.encrypt_enabled) printf("encryption: enabled (XChaCha20-Poly1305 payload encryption, cleartext authenticated talk_id)\n"); else printf("encryption: disabled\n");
    printf("ptt input: %s\n", g_app.codec_ptt_enabled ? "codec-ptt (KEY_ENTER)" : "keyboard Right Alt / AltGr"); if (!g_app.codec_ptt_enabled) printf("altgr ptt delay: %d ms\n", g_app.pc_ptt_hold_ms); if (g_app.ptt_socket_enabled) printf("ptt socket: %s\n", g_app.ptt_socket_path); if (g_app.state_file_enabled) printf("ptt state file: %s (rx timeout=%d ms)\n", g_app.state_file_path, g_app.rx_state_timeout_ms);
    if (g_app.alsa_capture_device[0]) printf("capture: alsasrc device=\"%s\"\n", g_app.alsa_capture_device); else printf("capture: autoaudiosrc\n");
    if (g_app.alsa_playback_device[0]) printf("playback: alsasink device=\"%s\"\n", g_app.alsa_playback_device); else printf("playback: autoaudiosink\n"); fflush(stdout);
    if (g_app.ptt_enabled) { if (pthread_create(&g_app.key_thread, NULL, keyboard_thread_main, &g_app) != 0) { perror("pthread_create(key_thread)"); cleanup(&g_app); return 1; } }
    if (g_app.ptt_socket_enabled) { if (pthread_create(&g_app.ctrl_thread, NULL, ptt_control_thread_main, &g_app) != 0) { perror("pthread_create(ctrl_thread)"); cleanup(&g_app); return 1; } }
    if (!(g_app.transport_mode == TRANSPORT_BLACKFIBER && g_app.blackfiber_rx_passive)) {
        if (pthread_create(&g_app.send_thread, NULL, send_thread_main, &g_app) != 0) {
            perror("pthread_create(send_thread)");
            cleanup(&g_app);
            return 1;
        }
    }
    if (!(g_app.transport_mode == TRANSPORT_BLACKFIBER && g_app.blackfiber_tx_only)) {
        if (pthread_create(&g_app.recv_thread, NULL, recv_thread_main, &g_app) != 0) {
            perror("pthread_create(recv_thread)");
            cleanup(&g_app);
            return 1;
        }
    }
    if (!(g_app.transport_mode == TRANSPORT_BLACKFIBER && g_app.blackfiber_tx_only)) {
        if (pthread_create(&g_app.play_thread, NULL, play_thread_main, &g_app) != 0) {
            perror("pthread_create(playback)"); cleanup(&g_app); return 1;
        }
    }
    int64_t next_report = mono_ms() + 5000;
    while (atomic_load(&g_app.running)) {
        output_telemetry(&g_app);
        check_audio_bus(&g_app, g_app.capture_pipeline);
        if (mono_ms() >= next_report) { report_rx_stats(&g_app); next_report = mono_ms() + 5000; }
        msleep_int(100);
    }
    cleanup(&g_app); return 0;
}
