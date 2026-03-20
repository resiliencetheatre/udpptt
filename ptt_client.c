/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <gst/app/app.h>
#include <gst/gst.h>
#include <linux/input.h>
#include <limits.h>
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SERVER_PORT 5000
#define LOCAL_PORT 0
#define MAX_PACKET 2048
#define PKT_IDLE 0
#define PKT_AUDIO 1
#define INPUT_SCAN_MAX 64
#define TALK_ID_MAX 31
#define PKT_FLAG_ENCRYPTED 0x01
#define NONCE_LEN crypto_aead_xchacha20poly1305_ietf_NPUBBYTES
#define KEY_LEN crypto_aead_xchacha20poly1305_ietf_KEYBYTES
#define AAD_LEN (1 + 1 + (TALK_ID_MAX + 1))
#define ALSA_DEV_MAX 128

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t flags;
    uint16_t len;
    char talk_id[TALK_ID_MAX + 1];
    uint8_t nonce[NONCE_LEN];
} packet_hdr_t;

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
    struct sockaddr_in server_addr;
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
    struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 200000; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static GstElement *make_capture_pipeline(GstElement **out_sink, const char *alsa_device) {
    GError *err = NULL; char desc[512];
    if (alsa_device && alsa_device[0]) snprintf(desc, sizeof(desc), "alsasrc device=\"%s\" ! audio/x-raw,format=S16LE,channels=1,rate=48000 ! audioconvert ! audioresample ! opusenc frame-size=20 bitrate=24000 audio-type=voice ! appsink name=capture_sink emit-signals=false sync=false max-buffers=8 drop=true", alsa_device);
    else snprintf(desc, sizeof(desc), "autoaudiosrc ! audio/x-raw,format=S16LE,channels=1,rate=48000 ! audioconvert ! audioresample ! opusenc frame-size=20 bitrate=24000 audio-type=voice ! appsink name=capture_sink emit-signals=false sync=false max-buffers=8 drop=true");
    GstElement *pipeline = gst_parse_launch(desc, &err);
    if (!pipeline || err) { fprintf(stderr, "capture pipeline error: %s\n", err ? err->message : "unknown"); if (err) g_error_free(err); if (pipeline) gst_object_unref(pipeline); return NULL; }
    *out_sink = gst_bin_get_by_name(GST_BIN(pipeline), "capture_sink"); if (!*out_sink) { fprintf(stderr, "failed to get capture_sink\n"); gst_object_unref(pipeline); return NULL; }
    gst_element_set_state(pipeline, GST_STATE_PLAYING); return pipeline;
}

static GstElement *make_playback_pipeline(GstElement **out_src, const char *alsa_device) {
    GError *err = NULL; char desc[512];
    if (alsa_device && alsa_device[0]) snprintf(desc, sizeof(desc), "appsrc name=play_src is-live=true format=time block=false do-timestamp=true ! opusdec ! audioconvert ! audioresample ! alsasink device=\"%s\" sync=false", alsa_device);
    else snprintf(desc, sizeof(desc), "appsrc name=play_src is-live=true format=time block=false do-timestamp=true ! opusdec ! audioconvert ! audioresample ! autoaudiosink");
    GstElement *pipeline = gst_parse_launch(desc, &err);
    if (!pipeline || err) { fprintf(stderr, "playback pipeline error: %s\n", err ? err->message : "unknown"); if (err) g_error_free(err); if (pipeline) gst_object_unref(pipeline); return NULL; }
    *out_src = gst_bin_get_by_name(GST_BIN(pipeline), "play_src"); if (!*out_src) { fprintf(stderr, "failed to get play_src\n"); gst_object_unref(pipeline); return NULL; }
    GstCaps *caps = gst_caps_new_simple("audio/x-opus", "rate", G_TYPE_INT, 48000, "channels", G_TYPE_INT, 1, "channel-mapping-family", G_TYPE_INT, 0, NULL);
    g_object_set(*out_src, "caps", caps, "stream-type", 0, "format", GST_FORMAT_TIME, NULL); gst_caps_unref(caps);
    gst_element_set_state(pipeline, GST_STATE_PLAYING); return pipeline;
}

static void make_aad(uint8_t type, uint8_t flags, const char talk_id[TALK_ID_MAX + 1], unsigned char aad[AAD_LEN]) { memset(aad, 0, AAD_LEN); aad[0] = type; aad[1] = flags; memcpy(aad + 2, talk_id, strnlen(talk_id, TALK_ID_MAX + 1)); }

static bool send_packet(app_t *app, uint8_t type, const uint8_t *payload, uint16_t plain_len) {
    uint8_t buf[MAX_PACKET]; packet_hdr_t hdr; memset(&hdr, 0, sizeof(hdr)); hdr.type = type; memcpy(hdr.talk_id, app->txid, strnlen(app->txid, TALK_ID_MAX + 1));
    unsigned long long wire_len = plain_len; const uint8_t *wire_payload = payload; unsigned char cipher[MAX_PACKET];
    if (type == PKT_AUDIO && app->encrypt_enabled) {
        hdr.flags |= PKT_FLAG_ENCRYPTED; randombytes_buf(hdr.nonce, sizeof(hdr.nonce)); unsigned char aad[AAD_LEN]; make_aad(hdr.type, hdr.flags, hdr.talk_id, aad);
        if (sizeof(hdr) + plain_len + crypto_aead_xchacha20poly1305_ietf_ABYTES > sizeof(buf) || plain_len + crypto_aead_xchacha20poly1305_ietf_ABYTES > sizeof(cipher)) { fprintf(stderr, "encrypted packet too large: %u\n", plain_len); return false; }
        if (crypto_aead_xchacha20poly1305_ietf_encrypt(cipher, &wire_len, payload, plain_len, aad, sizeof(aad), NULL, hdr.nonce, app->key) != 0) { fprintf(stderr, "encrypt failed\n"); return false; }
        wire_payload = cipher;
    }
    if (sizeof(hdr) + wire_len > sizeof(buf)) { fprintf(stderr, "packet too large: %llu\n", wire_len); return false; }
    hdr.len = htons((uint16_t)wire_len); memcpy(buf, &hdr, sizeof(hdr)); if (wire_len > 0 && wire_payload) memcpy(buf + sizeof(hdr), wire_payload, (size_t)wire_len);
    ssize_t sent = send(app->sock, buf, sizeof(hdr) + (size_t)wire_len, 0); if (sent < 0) { perror("send"); return false; } return true;
}

static bool play_opus_packet(app_t *app, const uint8_t *data, uint16_t len) {
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, len, NULL); if (!buffer) return false;
    gst_buffer_fill(buffer, 0, data, len); GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(app->playback_src), buffer); return ret == GST_FLOW_OK;
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
        printf("[%lld] PTT %s (txid=%s)\n", now_ms(), pressed ? "DOWN -> sending microphone audio" : "UP -> sending idle frames", app->txid); fflush(stdout);
        if (pressed) { if (app->have_start_wav) play_tone_async(app, app->start_wav_path); }
        else { if (app->have_stop_wav) play_tone_async(app, app->stop_wav_path); }
    }
}

static void update_effective_ptt_state(app_t *app) { int pressed = atomic_load(&app->ptt_keyboard_pressed) || atomic_load(&app->ptt_socket_pressed); set_ptt_state(app, pressed ? 1 : 0); }
static void set_keyboard_ptt_state(app_t *app, int pressed) { atomic_store(&app->ptt_keyboard_pressed, pressed ? 1 : 0); update_effective_ptt_state(app); }
static void set_socket_ptt_state(app_t *app, int pressed) { atomic_store(&app->ptt_socket_pressed, pressed ? 1 : 0); update_effective_ptt_state(app); }

static int make_ptt_control_socket(const char *path) {
    int fd;
    struct sockaddr_un addr;
    struct timeval tv;

    if (!path || !path[0]) {
        return -1;
    }
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "ptt socket path too long: %s\n", path);
        return -1;
    }

    unlink(path);

    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket(AF_UNIX)");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind(AF_UNIX)");
        close(fd);
        unlink(path);
        return -1;
    }

    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return fd;
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

static void *send_thread_main(void *arg) {
    app_t *app = (app_t *)arg; unsigned long last_report_audio = 0; unsigned long last_report_idle = 0;
    while (atomic_load(&app->running)) {
        if (app->ptt_enabled && atomic_load(&app->ptt_pressed)) {
            GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(app->capture_sink), 50 * GST_MSECOND);
            if (sample) {
                GstBuffer *buffer = gst_sample_get_buffer(sample); GstMapInfo map;
                if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                    if (send_packet(app, PKT_AUDIO, map.data, (uint16_t)map.size)) {
                        unsigned long n = atomic_fetch_add(&app->tx_audio_packets, 1) + 1;
                        if (n == 1 || n - last_report_audio >= 50) { printf("[%lld] TX mic audio packet #%lu (%zu bytes opus%s, txid=%s)\n", now_ms(), n, map.size, app->encrypt_enabled ? ", enc" : "", app->txid); fflush(stdout); last_report_audio = n; }
                    }
                    gst_buffer_unmap(buffer, &map);
                }
                gst_sample_unref(sample);
            } else { if (send_packet(app, PKT_IDLE, NULL, 0)) atomic_fetch_add(&app->tx_idle_packets, 1); msleep_int(20); }
        } else {
            if (send_packet(app, PKT_IDLE, NULL, 0)) {
                unsigned long n = atomic_fetch_add(&app->tx_idle_packets, 1) + 1;
                if (n == 1 || n - last_report_idle >= 200) { printf("[%lld] TX idle packet #%lu (txid=%s)\n", now_ms(), n, app->txid); fflush(stdout); last_report_idle = n; }
            }
            msleep_int(20);
        }
    }
    return NULL;
}

static void *recv_thread_main(void *arg) {
    app_t *app = (app_t *)arg; uint8_t buf[MAX_PACKET]; unsigned long last_report_rx = 0; unsigned long last_report_play = 0; char current_talker[TALK_ID_MAX + 1] = {0};
    while (atomic_load(&app->running)) {
        ssize_t n = recv(app->sock, buf, sizeof(buf), 0);
        if (n < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) continue; perror("recv"); break; }
        if ((size_t)n < sizeof(packet_hdr_t)) continue;
        packet_hdr_t hdr; memcpy(&hdr, buf, sizeof(hdr)); hdr.talk_id[TALK_ID_MAX] = '\0'; uint16_t len = ntohs(hdr.len); if ((size_t)n < sizeof(hdr) + len) continue;
        if (hdr.type == PKT_IDLE) { unsigned long idle_n = atomic_fetch_add(&app->rx_ignored_idle_packets, 1) + 1; if (idle_n == 1 || idle_n % 200 == 0) { printf("[%lld] RX idle packet ignored #%lu (from=%s)\n", now_ms(), idle_n, hdr.talk_id); fflush(stdout); } continue; }
        if (hdr.type != PKT_AUDIO || len == 0) continue;
        if (strcmp(current_talker, hdr.talk_id) != 0) { snprintf(current_talker, sizeof(current_talker), "%s", hdr.talk_id); printf("[%lld] TALKER now: %s\n", now_ms(), current_talker[0] ? current_talker : "?"); fflush(stdout); }
        unsigned char plain[MAX_PACKET]; const uint8_t *opus = buf + sizeof(hdr); unsigned long long opus_len = len;
        if (hdr.flags & PKT_FLAG_ENCRYPTED) {
            if (!app->encrypt_enabled) { unsigned long bad = atomic_fetch_add(&app->rx_decrypt_failures, 1) + 1; if (bad == 1 || bad % 50 == 0) { printf("[%lld] RX encrypted audio from=%s but local client is not in --encrypt mode\n", now_ms(), hdr.talk_id); fflush(stdout); } continue; }
            unsigned char aad[AAD_LEN]; make_aad(hdr.type, hdr.flags, hdr.talk_id, aad);
            if (crypto_aead_xchacha20poly1305_ietf_decrypt(plain, &opus_len, NULL, buf + sizeof(hdr), len, aad, sizeof(aad), hdr.nonce, app->key) != 0) {
                unsigned long bad = atomic_fetch_add(&app->rx_decrypt_failures, 1) + 1; if (bad == 1 || bad % 50 == 0) { printf("[%lld] RX decrypt/auth failure #%lu from=%s\n", now_ms(), bad, hdr.talk_id); fflush(stdout); } continue;
            }
            opus = plain;
        }
        unsigned long rx_n = atomic_fetch_add(&app->rx_audio_packets, 1) + 1;
        if (rx_n == 1 || rx_n - last_report_rx >= 50) { printf("[%lld] RX audio packet #%lu (%llu bytes opus%s, from=%s)\n", now_ms(), rx_n, opus_len, (hdr.flags & PKT_FLAG_ENCRYPTED) ? ", enc" : "", hdr.talk_id); fflush(stdout); last_report_rx = rx_n; }
        if (atomic_load(&app->suppress_playback)) { if (rx_n == 1 || rx_n % 50 == 0) { printf("[%lld] RX audio suppressed while local PTT is active (from=%s)\n", now_ms(), hdr.talk_id); fflush(stdout); } continue; }
        if (opus_len > UINT16_MAX) continue;
        if (play_opus_packet(app, opus, (uint16_t)opus_len)) { unsigned long play_n = atomic_fetch_add(&app->rx_played_packets, 1) + 1; if (play_n == 1 || play_n - last_report_play >= 50) { printf("[%lld] PLAY audio packet #%lu (from=%s)\n", now_ms(), play_n, hdr.talk_id); fflush(stdout); last_report_play = play_n; } }
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
    if (app->capture_pipeline) { gst_element_set_state(app->capture_pipeline, GST_STATE_NULL); if (app->capture_sink) gst_object_unref(app->capture_sink); gst_object_unref(app->capture_pipeline); }
    if (app->playback_pipeline) { gst_element_set_state(app->playback_pipeline, GST_STATE_NULL); if (app->playback_src) { gst_app_src_end_of_stream(GST_APP_SRC(app->playback_src)); gst_object_unref(app->playback_src); } gst_object_unref(app->playback_pipeline); }
    if (app->sock >= 0) { close(app->sock); app->sock = -1; }
    if (app->ptt_socket_enabled && app->ptt_socket_path[0]) unlink(app->ptt_socket_path);
    sodium_memzero(app->key, sizeof(app->key));
    printf("summary: tx_audio=%lu tx_idle=%lu rx_audio=%lu played=%lu rx_idle_ignored=%lu decrypt_fail=%lu\n",
           (unsigned long)atomic_load(&app->tx_audio_packets), (unsigned long)atomic_load(&app->tx_idle_packets),
           (unsigned long)atomic_load(&app->rx_audio_packets), (unsigned long)atomic_load(&app->rx_played_packets),
           (unsigned long)atomic_load(&app->rx_ignored_idle_packets), (unsigned long)atomic_load(&app->rx_decrypt_failures));
}

int main(int argc, char **argv) {
    memset(&g_app, 0, sizeof(g_app)); g_app.sock = -1; g_app.ptt_ctrl_sock = -1; g_app.server_port = DEFAULT_SERVER_PORT; g_app.ptt_enabled = 1; g_app.pc_ptt_hold_ms = 2000; snprintf(g_app.txid, sizeof(g_app.txid), "anon");
    const char *server_ip = NULL; const char *password = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--rx-only") || !strcmp(argv[i], "--no-ptt")) g_app.ptt_enabled = 0;
        else if (!strcmp(argv[i], "--encrypt")) g_app.encrypt_enabled = 1;
        else if (!strcmp(argv[i], "--codec-ptt")) g_app.codec_ptt_enabled = 1;
        else if (!strcmp(argv[i], "--ptt-socket") || !strcmp(argv[i], "--control-socket")) { if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", argv[i]); return 1; } copy_opt_string(g_app.ptt_socket_path, sizeof(g_app.ptt_socket_path), argv[++i]); g_app.ptt_socket_enabled = 1; }
        else if (!strcmp(argv[i], "--port") || !strcmp(argv[i], "--udp-port")) { int p; if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", argv[i]); return 1; } p = parse_udp_port(argv[++i]); if (p < 0) { fprintf(stderr, "invalid UDP port: %s\n", argv[i]); return 1; } g_app.server_port = p; }
        else if (!strcmp(argv[i], "--altgr-ptt-delay-ms")) { if (i + 1 >= argc) { fprintf(stderr, "missing value for --altgr-ptt-delay-ms\n"); return 1; } char *endp = NULL; long v = strtol(argv[++i], &endp, 10); if (!endp || *endp != '\0' || v < 0 || v > 60000) { fprintf(stderr, "invalid value for --altgr-ptt-delay-ms: %s\n", argv[i]); return 1; } g_app.pc_ptt_hold_ms = (int)v; }
        else if (!strcmp(argv[i], "--txid")) { if (i + 1 >= argc) { fprintf(stderr, "missing value for --txid\n"); return 1; } sanitize_txid(g_app.txid, sizeof(g_app.txid), argv[++i]); }
        else if (!strcmp(argv[i], "--key")) { if (i + 1 >= argc) { fprintf(stderr, "missing value for --key\n"); return 1; } password = argv[++i]; }
        else if (!strcmp(argv[i], "--alsa-device")) { if (i + 1 >= argc) { fprintf(stderr, "missing value for --alsa-device\n"); return 1; } copy_opt_string(g_app.alsa_capture_device, sizeof(g_app.alsa_capture_device), argv[++i]); copy_opt_string(g_app.alsa_playback_device, sizeof(g_app.alsa_playback_device), g_app.alsa_capture_device); }
        else if (!strcmp(argv[i], "--alsa-capture-device")) { if (i + 1 >= argc) { fprintf(stderr, "missing value for --alsa-capture-device\n"); return 1; } copy_opt_string(g_app.alsa_capture_device, sizeof(g_app.alsa_capture_device), argv[++i]); }
        else if (!strcmp(argv[i], "--alsa-playback-device")) { if (i + 1 >= argc) { fprintf(stderr, "missing value for --alsa-playback-device\n"); return 1; } copy_opt_string(g_app.alsa_playback_device, sizeof(g_app.alsa_playback_device), argv[++i]); }
        else if (!strcmp(argv[i], "--rpi-audio")) { copy_opt_string(g_app.alsa_capture_device, sizeof(g_app.alsa_capture_device), "plughw:0,0"); copy_opt_string(g_app.alsa_playback_device, sizeof(g_app.alsa_playback_device), "plughw:0,0"); }
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option: %s\nusage: %s <server-ip> [--port PORT] [--txid NAME] [--rx-only|--no-ptt] [--encrypt] [--key PASSWORD]\n       [--codec-ptt] [--ptt-socket PATH] [--altgr-ptt-delay-ms MS] [--rpi-audio]\n       [--alsa-device DEV] [--alsa-capture-device DEV] [--alsa-playback-device DEV]\n", argv[i], argv[0]); return 1; }
        else if (!server_ip) server_ip = argv[i];
        else { fprintf(stderr, "unexpected extra argument: %s\n", argv[i]); return 1; }
    }
    if (!server_ip) { fprintf(stderr, "usage: %s <server-ip> [--port PORT] [--txid NAME] [--rx-only|--no-ptt] [--encrypt] [--key PASSWORD]\n       [--codec-ptt] [--ptt-socket PATH] [--altgr-ptt-delay-ms MS] [--rpi-audio]\n       [--alsa-device DEV] [--alsa-capture-device DEV] [--alsa-playback-device DEV]\n", argv[0]); return 1; }
    if (g_app.ptt_socket_enabled && !g_app.ptt_socket_path[0]) { fprintf(stderr, "--ptt-socket requires a non-empty path\n"); return 1; }
    if (sodium_init() < 0) { fprintf(stderr, "libsodium init failed\n"); return 1; }
    if (g_app.encrypt_enabled) { if (!password || !password[0]) password = getenv("UDPPTT_KEY"); if (!password || !password[0]) { fprintf(stderr, "--encrypt requires --key PASSWORD or UDPPTT_KEY\n"); return 1; } if (derive_key_from_password(password, g_app.key) != 0) return 1; }
    signal(SIGINT, on_sigint); signal(SIGTERM, on_sigint); gst_init(&argc, &argv);
    g_app.sock = make_udp_socket(server_ip, g_app.server_port, &g_app.server_addr); if (g_app.sock < 0) return 1;
    if (g_app.ptt_socket_enabled) { g_app.ptt_ctrl_sock = make_ptt_control_socket(g_app.ptt_socket_path); if (g_app.ptt_ctrl_sock < 0) { cleanup(&g_app); return 1; } }
    if (g_app.ptt_enabled) { g_app.capture_pipeline = make_capture_pipeline(&g_app.capture_sink, g_app.alsa_capture_device); if (!g_app.capture_pipeline) { cleanup(&g_app); return 1; } }
    g_app.playback_pipeline = make_playback_pipeline(&g_app.playback_src, g_app.alsa_playback_device); if (!g_app.playback_pipeline) { cleanup(&g_app); return 1; }
    init_ptt_tones(&g_app);
    atomic_store(&g_app.running, 1); atomic_store(&g_app.ptt_pressed, 0); atomic_store(&g_app.ptt_keyboard_pressed, 0); atomic_store(&g_app.ptt_socket_pressed, 0); atomic_store(&g_app.suppress_playback, 0);
    printf("connected to %s:%d\n", server_ip, g_app.server_port); printf("txid: %s\n", g_app.txid); printf("debug: shows PTT state, transmitted mic packets, received audio packets, and playback events\n");
    if (!g_app.ptt_enabled) printf("mode: receive-only (--rx-only), keyboard PTT disabled, microphone capture disabled\n");
    if (g_app.encrypt_enabled) printf("encryption: enabled (XChaCha20-Poly1305 payload encryption, cleartext authenticated talk_id)\n"); else printf("encryption: disabled\n");
    printf("ptt input: %s\n", g_app.codec_ptt_enabled ? "codec-ptt (KEY_ENTER)" : "keyboard Right Alt / AltGr"); if (!g_app.codec_ptt_enabled) printf("altgr ptt delay: %d ms\n", g_app.pc_ptt_hold_ms); if (g_app.ptt_socket_enabled) printf("ptt socket: %s\n", g_app.ptt_socket_path);
    if (g_app.alsa_capture_device[0]) printf("capture: alsasrc device=\"%s\"\n", g_app.alsa_capture_device); else printf("capture: autoaudiosrc\n");
    if (g_app.alsa_playback_device[0]) printf("playback: alsasink device=\"%s\"\n", g_app.alsa_playback_device); else printf("playback: autoaudiosink\n"); fflush(stdout);
    if (g_app.ptt_enabled) { if (pthread_create(&g_app.key_thread, NULL, keyboard_thread_main, &g_app) != 0) { perror("pthread_create(key_thread)"); cleanup(&g_app); return 1; } }
    if (g_app.ptt_socket_enabled) { if (pthread_create(&g_app.ctrl_thread, NULL, ptt_control_thread_main, &g_app) != 0) { perror("pthread_create(ctrl_thread)"); cleanup(&g_app); return 1; } }
    if (pthread_create(&g_app.send_thread, NULL, send_thread_main, &g_app) != 0) { perror("pthread_create(send_thread)"); cleanup(&g_app); return 1; }
    if (pthread_create(&g_app.recv_thread, NULL, recv_thread_main, &g_app) != 0) { perror("pthread_create(recv_thread)"); cleanup(&g_app); return 1; }
    while (atomic_load(&g_app.running)) msleep_int(100);
    cleanup(&g_app); return 0;
}
