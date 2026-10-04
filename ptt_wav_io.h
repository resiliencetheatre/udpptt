/* SPDX-License-Identifier: GPL-3.0-or-later */
/* File audio backend, included by the shared client implementation. */
#include <dirent.h>
#include <sys/file.h>
#include <sys/syscall.h>

static char wav_input[PATH_MAX], wav_output[PATH_MAX], wav_sent[PATH_MAX];
static atomic_int wav_failed;

static int wav_directory(char path[PATH_MAX]) {
    if (mkdir(path, 0700) && errno != EEXIST) { perror(path); return -1; }
    char resolved[PATH_MAX]; struct stat st;
    if (!realpath(path, resolved) || stat(resolved, &st) || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "not a directory: %s\n", path); return -1;
    }
    strcpy(path, resolved); return 0;
}

static int wav_setup(void) {
    if (!wav_input[0] || !wav_output[0]) {
        fprintf(stderr, "WAV mode requires --input-dir and --output-dir\n"); return -1;
    }
    if (wav_directory(wav_input) || wav_directory(wav_output)) return -1;
    if (!strcmp(wav_input, wav_output)) {
        fprintf(stderr, "input and output directories must differ\n"); return -1;
    }
    if (snprintf(wav_sent, sizeof(wav_sent), "%s/sent", wav_input) >= PATH_MAX || wav_directory(wav_sent)) return -1;
    if (!strcmp(wav_sent, wav_input) || !strcmp(wav_sent, wav_output)) {
        fprintf(stderr, "sent directory must differ from input/output\n"); return -1;
    }
    /* Lifetime lock: prevent two gateways consuming the same queue. */
    int fd = open(wav_input, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB)) {
        fprintf(stderr, "cannot lock input directory: %s\n", strerror(errno));
        if (fd >= 0) close(fd);
        return -1;
    }
    return 0;
}

static int wav_same(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

/* Decode into a disk-backed snapshot before transmitting. No sound device is
 * opened; wavparse rejects non-WAV input. appsink backpressure bounds memory. */
static FILE *wav_decode(app_t *app, int fd) {
    unsigned char magic[12];
    if (pread(fd, magic, sizeof(magic), 0) != sizeof(magic) ||
        memcmp(magic, "RIFF", 4) || memcmp(magic + 8, "WAVE", 4)) return NULL;
    struct stat st;
    uint32_t riff_size = (uint32_t)magic[4] | (uint32_t)magic[5] << 8 |
        (uint32_t)magic[6] << 16 | (uint32_t)magic[7] << 24;
    if (fstat(fd, &st) || riff_size < 36 || (uint64_t)riff_size + 8 > (uint64_t)st.st_size) return NULL;
    FILE *pcm = tmpfile();
    if (!pcm) return NULL;
    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch(
        "fdsrc name=input ! wavparse ! audioconvert ! audioresample ! "
        "audio/x-raw,format=" GST_AUDIO_NE(S16) ",rate=48000,channels=1,layout=interleaved ! "
        "appsink name=pcm sync=false max-buffers=8 drop=false", &error);
    if (!pipeline || error) {
        fprintf(stderr, "WAV decoder: %s\n", error ? error->message : "unavailable");
        g_clear_error(&error); if (pipeline) gst_object_unref(pipeline); fclose(pcm); return NULL;
    }
    GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "input");
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "pcm");
    GstBus *bus = gst_element_get_bus(pipeline);
    g_object_set(source, "fd", fd, NULL);
    int ok = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
    int64_t progress = mono_ms(), idle = progress;
    size_t total = 0;
    while (ok && atomic_load(&app->running)) {
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 50 * GST_MSECOND);
        if (sample) {
            GstMapInfo map;
            GstBuffer *buffer = gst_sample_get_buffer(sample);
            if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) ok = 0;
            else {
                if (fwrite(map.data, 1, map.size, pcm) != map.size) ok = 0;
                total += map.size;
                gst_buffer_unmap(buffer, &map);
            }
            gst_sample_unref(sample); progress = mono_ms();
        }
        GstMessage *msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        if (msg) {
            gchar *debug = NULL; gst_message_parse_error(msg, &error, &debug);
            fprintf(stderr, "WAV decode: %s\n", error->message);
            g_clear_error(&error); g_free(debug); gst_message_unref(msg); ok = 0;
        }
        if (mono_ms() - idle >= 100) { send_packet(app, PKT_IDLE, NULL, 0); idle = mono_ms(); }
        if (!sample && gst_app_sink_is_eos(GST_APP_SINK(sink))) break;
        if (mono_ms() - progress > 5000) ok = 0;
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(source); gst_object_unref(sink); gst_object_unref(bus); gst_object_unref(pipeline);
    if (!ok || !atomic_load(&app->running) || !total || total % 2 || fflush(pcm) || fseek(pcm, 0, SEEK_SET)) {
        fclose(pcm); return NULL;
    }
    return pcm;
}

static int wav_send_pcm(app_t *app, FILE *pcm) {
    int error;
    OpusEncoder *encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error);
    if (!encoder) return 0;
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(app->fec_enabled));
    opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(app->loss_percent));
    tm_burst burst; unsigned preamble = 0;
    if (app->preamble_enabled) {
        tm_record record = app->telemetry_tx; read_fix(app, &record);
        record.sequence = app->telemetry_tx.sequence++;
        if (!tm_build(&burst, &record)) { opus_encoder_destroy(encoder); return 0; }
        preamble = burst.frames;
    }
    randombytes_buf(app->tx_session, sizeof(app->tx_session)); app->tx_seq = 0;
    set_ptt_state(app, 1);
    int ok = 1; unsigned frame = 0;
    int64_t due = mono_ms();
    while (atomic_load(&app->running)) {
        int16_t samples[PTT_SAMPLES] = {0};
        if (frame < preamble) tm_frame(&burst, frame++, samples);
        else {
            size_t n = fread(samples, sizeof(samples[0]), PTT_SAMPLES, pcm);
            if (ferror(pcm)) { ok = 0; break; }
            if (!n) break;
        }
        while (atomic_load(&app->running) && mono_ms() < due) msleep_int(2);
        if (!atomic_load(&app->running)) break;
        uint8_t packet[PTT_OPUS_MAX];
        int n = opus_encode(encoder, samples, PTT_SAMPLES, packet, sizeof(packet));
        if (n <= 0 || !send_packet(app, PKT_AUDIO, packet, (uint16_t)n)) { ok = 0; break; }
        atomic_fetch_add(&app->tx_audio_packets, 1);
        int64_t now = mono_ms();
        if (now - due >= PTT_FRAME_MS) due = now;
        due += PTT_FRAME_MS;
    }
    /* Keep END behind the final frame in real time, as a button release is. */
    while (atomic_load(&app->running) && mono_ms() < due) msleep_int(2);
    if (!atomic_load(&app->running)) ok = 0;
    for (int i = 0; i < 3; i++) if (!send_packet(app, PKT_END, NULL, 0)) ok = 0;
    set_ptt_state(app, 0);
    opus_encoder_destroy(encoder); return ok;
}

/* Keep the original name when available, otherwise archive as name_1.wav,
 * name_2.wav, etc. RENAME_NOREPLACE also handles destinations created during TX. */
static int wav_archive(const char *path, const char *name, char target[PATH_MAX]) {
    unsigned suffix = 0;
    do {
        int n;
        if (!suffix) n = snprintf(target, PATH_MAX, "%s/%s", wav_sent, name);
        else {
            size_t stem = strlen(name) - 4;
            /* Leave room for underscore, ten decimal digits and .wav. */
            if (stem > NAME_MAX - 15) stem = NAME_MAX - 15;
            n = snprintf(target, PATH_MAX, "%s/%.*s_%u.wav", wav_sent, (int)stem, name, suffix);
        }
        if (n < 0 || n >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
        if (!syscall(SYS_renameat2, AT_FDCWD, path, AT_FDCWD, target, 1 /* RENAME_NOREPLACE */)) return 0;
        if (errno != EEXIST) return -1;
    } while (++suffix);
    errno = EOVERFLOW;
    return -1;
}

typedef struct { struct stat st; int64_t seen; int rejected; } wav_candidate;

static void *wav_send_thread(void *arg) {
    app_t *app = arg;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    int64_t scan_due = 0;
    while (atomic_load(&app->running)) {
        if (send_packet(app, PKT_IDLE, NULL, 0)) atomic_fetch_add(&app->tx_idle_packets, 1);
        if (!app->ptt_enabled || mono_ms() < scan_due) { msleep_int(20); continue; }
        scan_due = mono_ms() + 250;
        struct dirent **entries = NULL;
        int count = scandir(wav_input, &entries, NULL, alphasort);
        if (count < 0) { perror("scan WAV input"); atomic_store(&wav_failed, 1); atomic_store(&app->running, 0); break; }
        for (int i = 0; i < count; i++) {
            char *name = entries[i]->d_name;
            size_t len = strlen(name);
            if (!atomic_load(&app->running) || name[0] == '.' || len < 5 || strcasecmp(name + len - 4, ".wav")) { free(entries[i]); continue; }
            char path[PATH_MAX], target[PATH_MAX]; struct stat st, after;
            if (snprintf(path, sizeof(path), "%s/%s", wav_input, name) >= PATH_MAX ||
                snprintf(target, sizeof(target), "%s/%s", wav_sent, name) >= PATH_MAX ||
                lstat(path, &st) || !S_ISREG(st.st_mode)) { free(entries[i]); continue; }
            wav_candidate *candidate = g_hash_table_lookup(seen, name);
            if (!candidate) {
                candidate = g_new0(wav_candidate, 1);
                g_hash_table_insert(seen, g_strdup(name), candidate);
            }
            if (!wav_same(&st, &candidate->st)) {
                candidate->st = st; candidate->seen = mono_ms(); candidate->rejected = 0;
            }
            if (candidate->rejected || mono_ms() - candidate->seen < 1000) { free(entries[i]); continue; }
            int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            FILE *pcm = NULL;
            if (fd >= 0 && !fstat(fd, &after) && wav_same(&st, &after)) pcm = wav_decode(app, fd);
            int unchanged = fd >= 0 && !fstat(fd, &after) && wav_same(&st, &after);
            if (fd >= 0) close(fd);
            if (!pcm || !unchanged) {
                fprintf(stderr, "invalid or changing WAV; leaving queued: %s\n", path);
                if (pcm) fclose(pcm);
                candidate->rejected = 1; free(entries[i]); continue;
            }
            printf("sending WAV: %s\n", path); fflush(stdout);
            int ok = wav_send_pcm(app, pcm); fclose(pcm);
            if (ok) {
                if (lstat(path, &after) || !wav_same(&st, &after) ||
                    wav_archive(path, name, target)) {
                    fprintf(stderr, "sent but could not archive %s; stopping to avoid duplicate sends\n", path);
                    atomic_store(&wav_failed, 1); atomic_store(&app->running, 0);
                } else {
                    printf("sent WAV: %s\n", target); fflush(stdout);
                    g_hash_table_remove(seen, name);
                }
            } else {
                fprintf(stderr, "send interrupted/failed; WAV retained: %s\n", path);
                candidate->seen = mono_ms() + 4000;
            }
            free(entries[i]);
        }
        free(entries);
    }
    g_hash_table_destroy(seen); return NULL;
}

static void wav_le16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void wav_le32(uint8_t *p, uint32_t v) { wav_le16(p, v); wav_le16(p + 2, v >> 16); }
static int wav_header(FILE *f, uint32_t bytes) {
    uint8_t h[44] = {0};
    memcpy(h, "RIFF", 4); wav_le32(h + 4, 36 + bytes); memcpy(h + 8, "WAVEfmt ", 8);
    wav_le32(h + 16, 16); wav_le16(h + 20, 1); wav_le16(h + 22, 1);
    wav_le32(h + 24, 48000); wav_le32(h + 28, 96000); wav_le16(h + 32, 2); wav_le16(h + 34, 16);
    memcpy(h + 36, "data", 4); wav_le32(h + 40, bytes);
    return fseek(f, 0, SEEK_SET) || fwrite(h, 1, sizeof(h), f) != sizeof(h) ? -1 : 0;
}

static int wav_publish(FILE *f, uint32_t bytes, const char *temp, const char *final) {
    int bad = wav_header(f, bytes);
    if (fflush(f) || fsync(fileno(f))) bad = 1;
    if (fclose(f)) bad = 1;
    if (!bad && syscall(SYS_renameat2, AT_FDCWD, temp, AT_FDCWD, final, 1) == 0) {
        printf("received WAV: %s\n", final); fflush(stdout); return 0;
    }
    fprintf(stderr, "cannot finalize received WAV; retained %s: %s\n", temp, strerror(errno)); return -1;
}

typedef struct {
    FILE *file;
    uint32_t bytes;
    uint8_t session[16];
    char temp[PATH_MAX], final[PATH_MAX];
} wav_recording;

typedef struct {
    int rendered[PTT_STREAMS];
    int16_t pcm[PTT_STREAMS][PTT_SAMPLES];
} wav_frames;

static void wav_collect(void *context, unsigned slot, const int16_t pcm[PTT_SAMPLES]) {
    wav_frames *frames = context;
    frames->rendered[slot] = 1;
    memcpy(frames->pcm[slot], pcm, sizeof(frames->pcm[slot]));
}

static int wav_close_recording(wav_recording *r) {
    if (!r->file) return 0;
    int result = wav_publish(r->file, r->bytes, r->temp, r->final);
    r->file = NULL;
    return result;
}

static int wav_open_recording(wav_recording *r, const uint8_t session[16]) {
    struct timespec ts; struct tm utc; char stamp[40];
    clock_gettime(CLOCK_REALTIME, &ts); gmtime_r(&ts.tv_sec, &utc);
    strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%S", &utc);
    int n = snprintf(r->temp, sizeof(r->temp), "%s/.rx_%s_%09ld_XXXXXX.part", wav_output, stamp, ts.tv_nsec);
    if (n < 0 || n >= PATH_MAX - 5) { errno = ENAMETOOLONG; return -1; }
    int fd = mkstemps(r->temp, 5);
    if (fd < 0) return -1;
    r->file = fdopen(fd, "w+b");
    if (!r->file) { close(fd); return -1; }
    strcpy(r->final, r->temp);
    char *base = strrchr(r->final, '/') + 1;
    memmove(base, base + 1, strlen(base));
    strcpy(r->final + strlen(r->final) - 5, ".wav");
    r->bytes = 0;
    memcpy(r->session, session, 16);
    return wav_header(r->file, 0);
}

static void *wav_receive_thread(void *arg) {
    app_t *app = arg;
    wav_recording recordings[PTT_STREAMS] = {0};
    int64_t due = mono_ms();
    int failed = 0;
    while (atomic_load(&app->running) && !failed) {
        int64_t now = mono_ms();
        if (now < due) { msleep_int(2); continue; }
        if (now - due >= PTT_FRAME_MS) due = now;
        due += PTT_FRAME_MS;
        int16_t mixed[PTT_SAMPLES];
        wav_frames frames = {0};
        int active[PTT_STREAMS]; uint8_t sessions[PTT_STREAMS][16];
        pthread_mutex_lock(&app->jitter_lock);
        if (atomic_load(&app->suppress_playback)) ptt_jitter_suppress(&app->jitter);
        else ptt_jitter_render_each(&app->jitter, now, mixed, wav_collect, &frames);
        for (int i = 0; i < PTT_STREAMS; i++) {
            active[i] = app->jitter.streams[i].active;
            memcpy(sessions[i], app->jitter.streams[i].session, 16);
        }
        pthread_mutex_unlock(&app->jitter_lock);
        /* Disk I/O happens outside the jitter lock. Each remote PTT session has
         * its own file, including overlapping or immediately adjacent calls. */
        for (int i = 0; i < PTT_STREAMS && !failed; i++) {
            wav_recording *r = &recordings[i];
            if (r->file && (!active[i] || memcmp(r->session, sessions[i], 16))) {
                if (wav_close_recording(r)) { failed = 1; break; }
            }
            if (frames.rendered[i] && !r->file && wav_open_recording(r, sessions[i])) {
                failed = 1; break;
            }
            if (r->file) {
                uint8_t little[PTT_SAMPLES * 2];
                for (int k = 0; k < PTT_SAMPLES; k++)
                    wav_le16(little + 2 * k, frames.rendered[i] ? (uint16_t)frames.pcm[i][k] : 0);
                if (fwrite(little, 1, sizeof(little), r->file) != sizeof(little)) { failed = 1; break; }
                r->bytes += sizeof(little);
                atomic_fetch_add(&app->rx_played_packets, 1);
                /* Rotate below RIFF's 4 GiB limit, bounding STT file size. */
                if (r->bytes >= 48000u * 2 * 60 * 30 && wav_close_recording(r)) failed = 1;
            }
        }
        state_file_check_rx_timeout(app);
    }
    for (int i = 0; i < PTT_STREAMS; i++) {
        if (failed) {
            if (recordings[i].file) fclose(recordings[i].file);
        } else if (wav_close_recording(&recordings[i])) failed = 1;
    }
    if (failed) {
        fprintf(stderr, "WAV receive output failed; unfinished .part files retained\n");
        atomic_store(&wav_failed, 1); atomic_store(&app->running, 0);
    }
    return NULL;
}
