/* SPDX-License-Identifier: GPL-3.0-or-later */
#define main ptt_client_program_main
#include "../ptt_client.c"
#undef main
#include <assert.h>

static packet_hdr_t wait_type(int sock, unsigned type) {
    int64_t deadline = mono_ms() + 2000;
    while (mono_ms() < deadline) {
        uint8_t wire[MAX_PACKET];
        ssize_t n = recv(sock, wire, sizeof(wire), 0);
        if (n < 0) { assert(errno == EAGAIN || errno == EWOULDBLOCK); continue; }
        packet_hdr_t h; assert((size_t)n >= sizeof(h)); memcpy(&h, wire, sizeof(h));
        assert(ptt_header_valid(&h, (size_t)n));
        if (h.type == type) return h;
    }
    assert(!"timed out waiting for sender packet");
    return (packet_hdr_t){0};
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv); assert(sodium_init() >= 0);
    app_t *tx = calloc(1, sizeof(*tx)), *rx = calloc(1, sizeof(*rx));
    assert(tx && rx);
    pthread_mutex_init(&tx->jitter_lock, NULL);
    pthread_mutex_init(&tx->state_lock, NULL);
    pthread_mutex_init(&rx->jitter_lock, NULL);
    pthread_mutex_init(&rx->state_lock, NULL);
    ptt_jitter_init(&rx->jitter, 60, 1);
    int pair[2]; assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0);
    struct timeval timeout = {.tv_usec = 20000};
    assert(setsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    tx->sock = pair[0]; rx->sock = pair[1];
    tx->encrypt_enabled = rx->encrypt_enabled = 1;
    memcpy(tx->txid, "test", 5); randombytes_buf(tx->tx_session, 16);
    randombytes_buf(tx->key, sizeof(tx->key)); memcpy(rx->key, tx->key, sizeof(tx->key));
    const uint8_t silence[] = {0xf8, 0xff, 0xfe};
    assert(send_packet(tx, PKT_AUDIO, silence, sizeof(silence)));
    uint8_t wire[MAX_PACKET]; ssize_t n = recv(pair[1], wire, sizeof(wire), 0);
    assert(n > (ssize_t)sizeof(packet_hdr_t));
    packet_hdr_t h; memcpy(&h, wire, sizeof(h));
    assert(ptt_header_valid(&h, (size_t)n));
    unsigned char plain[MAX_PACKET]; unsigned long long len;
    assert(crypto_aead_xchacha20poly1305_ietf_decrypt(plain, &len, NULL,
        wire + sizeof(h), ntohs(h.len), (unsigned char *)&h, sizeof(h), h.nonce, tx->key) == 0);
    assert(len == sizeof(silence) && !memcmp(plain, silence, len));
    /* Every header byte, including sequence, timestamp, session and length,
     * participates in authentication. */
    for (size_t i = 0; i < sizeof(h); ++i) {
        packet_hdr_t changed = h; ((uint8_t *)&changed)[i] ^= 1;
        assert(crypto_aead_xchacha20poly1305_ietf_decrypt(plain, &len, NULL,
            wire + sizeof(h), ntohs(h.len), (unsigned char *)&changed, sizeof(changed), changed.nonce, tx->key) != 0);
    }
    atomic_store(&rx->running, 1);
    assert(pthread_create(&rx->recv_thread, NULL, recv_thread_main, rx) == 0);
    assert(send(pair[0], wire, n, 0) == n);
    assert(send(pair[0], wire, n, 0) == n); /* authenticated duplicate */
    wire[sizeof(h)] ^= 1; assert(send(pair[0], wire, n, 0) == n);
    assert(send_packet(tx, PKT_END, NULL, 0));
    msleep_int(100);
    atomic_store(&rx->running, 0); pthread_join(rx->recv_thread, NULL);
    assert(atomic_load(&rx->rx_audio_packets) == 2);
    assert(atomic_load(&rx->rx_decrypt_failures) == 1);
    assert(rx->jitter.stats.accepted == 1 && rx->jitter.stats.duplicates == 1);
    int have_end = 0;
    for (int i = 0; i < PTT_STREAMS; ++i)
        if (rx->jitter.streams[i].used && !memcmp(rx->jitter.streams[i].session, tx->tx_session, 16))
            have_end = rx->jitter.streams[i].have_end;
    assert(have_end);

    /* The same encrypted packet traverses the Blackfiber frame extractor.
     * This uses a datagram socket fixture, not raw-interface privileges. */
    rx->transport_mode = TRANSPORT_BLACKFIBER;
    rx->bf_ethertype = DEFAULT_BLACKFIBER_ETHERTYPE;
    memset(rx->bf_local_mac, 0xaa, ETH_ALEN);
    uint8_t frame[MAX_FRAME] = {0}, extracted[MAX_PACKET];
    bf_eth_hdr_t eth = {0};
    memset(eth.src, 0xbb, ETH_ALEN); eth.ethertype = htons(rx->bf_ethertype);
    memcpy(frame, &eth, sizeof(eth));
    wire[sizeof(h)] ^= 1; /* restore authenticated payload */
    memcpy(frame + sizeof(eth), wire, (size_t)n);
    assert(send(pair[0], frame, sizeof(eth) + n, 0) == (ssize_t)sizeof(eth) + n);
    assert(recv_transport_packet(rx, extracted, sizeof(extracted)) == n);
    assert(!memcmp(extracted, wire, (size_t)n));
    memcpy(eth.src, rx->bf_local_mac, ETH_ALEN); memcpy(frame, &eth, sizeof(eth));
    assert(send(pair[0], frame, sizeof(eth) + n, 0) == (ssize_t)sizeof(eth) + n);
    assert(recv_transport_packet(rx, extracted, sizeof(extracted)) == -2);

    /* Verify the real capture pipeline enables FEC and emits 20 ms Opus. */
    GstElement *capture_sink = NULL;
    GstElement *capture = make_capture_pipeline(&capture_sink, "null", 1, 10, 0);
    assert(capture && capture_sink);
    GstElement *encoder = gst_bin_get_by_name(GST_BIN(capture), "opusenc0");
    assert(encoder);
    gboolean fec = FALSE; gint loss = 0;
    g_object_get(encoder, "inband-fec", &fec, "packet-loss-percentage", &loss, NULL);
    assert(fec && loss == 10); gst_object_unref(encoder);
    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(capture_sink), 2 * GST_SECOND);
    assert(sample);
    GstMapInfo map; GstBuffer *captured = gst_sample_get_buffer(sample);
    assert(gst_buffer_map(captured, &map, GST_MAP_READ));
    assert(opus_packet_get_nb_samples(map.data, (opus_int32)map.size, 48000) == PTT_SAMPLES);
    gst_buffer_unmap(captured, &map); gst_sample_unref(sample);

    /* Real capture -> sender: fresh session per PTT, increasing media time,
     * and an exclusive END boundary emitted on release. */
    tx->capture_sink = capture_sink; tx->ptt_enabled = 1;
    atomic_store(&tx->running, 1);
    set_ptt_state(tx, 1);
    assert(pthread_create(&tx->send_thread, NULL, send_thread_main, tx) == 0);
    packet_hdr_t first = wait_type(pair[1], PKT_AUDIO);
    packet_hdr_t second = wait_type(pair[1], PKT_AUDIO);
    assert(ntohl(first.seq) == 0);
    assert(ptt_seq_diff(ntohl(second.seq), ntohl(first.seq)) > 0);
    assert(!memcmp(first.session, second.session, 16));
    set_ptt_state(tx, 0);
    packet_hdr_t end = wait_type(pair[1], PKT_END);
    assert(!memcmp(first.session, end.session, 16));
    assert(ptt_seq_diff(ntohl(end.seq), ntohl(second.seq)) > 0);
    set_ptt_state(tx, 1);
    second = wait_type(pair[1], PKT_AUDIO);
    assert(ntohl(second.seq) == 0 && memcmp(first.session, second.session, 16));
    set_ptt_state(tx, 0);
    wait_type(pair[1], PKT_END);
    atomic_store(&tx->running, 0); pthread_join(tx->send_thread, NULL);
    gst_element_set_state(capture, GST_STATE_NULL);
    gst_object_unref(capture_sink); gst_object_unref(capture);

    /* Deferred USB capture stays NULL while idle, closes on release/error,
     * and only retries after a fresh press (also covers raw preamble capture). */
    for (int raw = 0; raw < 2; raw++) {
        tx->usbptt = 1; tx->capture_generation = 0; tx->capture_active = 0;
        tx->capture_pipeline = raw ? make_raw_capture_pipeline(&tx->capture_sink, "null", 1)
            : make_capture_pipeline(&tx->capture_sink, "null", 1, 10, 1);
        GstState state;
        update_usb_capture(tx);
        gst_element_get_state(tx->capture_pipeline, &state, NULL, GST_SECOND);
        assert(state == GST_STATE_NULL && !tx->capture_active);
        set_ptt_state(tx, 1); update_usb_capture(tx);
        assert(tx->capture_active);
        sample = gst_app_sink_try_pull_sample(GST_APP_SINK(tx->capture_sink), 2 * GST_SECOND);
        assert(sample); gst_sample_unref(sample);
        GError *error = g_error_new_literal(GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_READ, "simulated USB release EIO");
        assert(gst_element_post_message(tx->capture_pipeline,
            gst_message_new_error(GST_OBJECT(tx->capture_pipeline), error, "test")));
        g_error_free(error);
        update_usb_capture(tx); assert(!tx->capture_active);
        update_usb_capture(tx); assert(!tx->capture_active);
        set_ptt_state(tx, 0); update_usb_capture(tx);
        set_ptt_state(tx, 1); update_usb_capture(tx); assert(tx->capture_active);
        sample = gst_app_sink_try_pull_sample(GST_APP_SINK(tx->capture_sink), 2 * GST_SECOND);
        assert(sample); gst_sample_unref(sample);
        set_ptt_state(tx, 0); update_usb_capture(tx);
        gst_element_get_state(tx->capture_pipeline, &state, NULL, GST_SECOND);
        assert(state == GST_STATE_NULL && !tx->capture_active);
        gst_object_unref(tx->capture_sink); gst_object_unref(tx->capture_pipeline);
        tx->capture_sink = tx->capture_pipeline = NULL;
    }
    tx->usbptt = 0;

    /* GPS is an atomic snapshot file; missing/stale/malformed fixes fall back
     * to ID only rather than emitting a fabricated coordinate. */
    char fix_path[] = "/tmp/udpptt-fix-XXXXXX"; int fix_fd = mkstemp(fix_path); assert(fix_fd >= 0);
    snprintf(tx->gps_file, sizeof(tx->gps_file), "%s", fix_path);
    char fix_text[128]; int fix_len = snprintf(fix_text, sizeof(fix_text), "49.61160 6.13190 %.3f\n", now_ms()/1000. - 2);
    assert(write(fix_fd, fix_text, (size_t)fix_len) == fix_len);
    tm_record fix_record = {0}; read_fix(tx, &fix_record);
    assert(fix_record.gps && fix_record.latitude == 4961160 && fix_record.longitude == 613190 && fix_record.age >= 2);
    assert(ftruncate(fix_fd, 0) == 0); assert(lseek(fix_fd, 0, SEEK_SET) == 0);
    assert(write(fix_fd, "0 0 1\n", 6) == 6); read_fix(tx, &fix_record); assert(!fix_record.gps);
    close(fix_fd); unlink(fix_path); tx->gps_file[0] = 0;

    /* Machine-readable events use a separate nonblocking datagram socket. */
    char event_dir[] = "/tmp/udpptt-events-XXXXXX"; assert(mkdtemp(event_dir));
    struct sockaddr_un event_address = {.sun_family = AF_UNIX};
    snprintf(event_address.sun_path, sizeof(event_address.sun_path), "%s/listener", event_dir);
    int event_listener = socket(AF_UNIX, SOCK_DGRAM, 0); assert(event_listener >= 0);
    assert(bind(event_listener, (struct sockaddr *)&event_address, sizeof(event_address)) == 0);
    assert(setsockopt(event_listener, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    rx->telemetry_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0); assert(rx->telemetry_fd >= 0);
    snprintf(rx->telemetry_socket, sizeof(rx->telemetry_socket), "%s", event_address.sun_path);
    rx->jitter.events[rx->jitter.event_write++ % 16] = (tm_record){.id="ALPHA", .sequence=17, .gps=1, .latitude=4961160, .longitude=613190, .age=2};
    output_telemetry(rx);
    char event_json[512]; n = recv(event_listener, event_json, sizeof(event_json)-1, 0); assert(n > 0); event_json[n] = 0;
    assert(strstr(event_json, "\"id\":\"ALPHA\"") && strstr(event_json, "\"latitude\":49.61160"));
    assert(rx->jitter.event_read == rx->jitter.event_write);
    close(rx->telemetry_fd); rx->telemetry_fd = -1; close(event_listener);
    unlink(event_address.sun_path); rmdir(event_dir);

    /* Preamble uses the real sender/encoder and clocked local monitor. Verify
     * early release, a new burst on re-press, CRC-decoded audio and ready gate. */
    tx->preamble_enabled = 1; assert(tm_id(tx->telemetry_tx.id, "ALPHA"));
    snprintf(tx->alsa_playback_device, sizeof(tx->alsa_playback_device), "null");
    tx->capture_pipeline = make_raw_capture_pipeline(&tx->capture_sink, "null", 0);
    tx->playback_pipeline = make_playback_pipeline(&tx->playback_src, "null");
    assert(tx->capture_pipeline && tx->playback_pipeline);
    ptt_jitter_init(&tx->jitter, 40, 1);
    atomic_store(&tx->running, 1);
    assert(pthread_create(&tx->play_thread, NULL, play_thread_main, tx) == 0);
    assert(pthread_create(&tx->send_thread, NULL, send_thread_main, tx) == 0);
    set_ptt_state(tx, 1);
    first = wait_type(pair[1], PKT_AUDIO);
    assert(ntohl(first.seq) == 0 && !atomic_load(&tx->microphone_ready));
    set_ptt_state(tx, 0); wait_type(pair[1], PKT_END);
    assert(!atomic_load(&tx->microphone_ready));
    set_ptt_state(tx, 1);
    tm_decoder telemetry = {0}; tm_record event = {0}; int events = 0, decode_error;
    OpusDecoder *wire_decoder = opus_decoder_create(48000, 1, &decode_error); assert(wire_decoder);
    int64_t started = mono_ms(), deadline = started + 6000;
    while (mono_ms() < deadline) {
        n = recv(pair[1], wire, sizeof(wire), 0);
        if (n < 0) continue;
        memcpy(&h, wire, sizeof(h));
        if (h.type != PKT_AUDIO) continue;
        assert(crypto_aead_xchacha20poly1305_ietf_decrypt(plain, &len, NULL,
            wire + sizeof(h), ntohs(h.len), (unsigned char *)&h, sizeof(h), h.nonce, tx->key) == 0);
        int16_t decoded_pcm[PTT_SAMPLES];
        assert(opus_decode(wire_decoder, plain, (opus_int32)len, decoded_pcm, PTT_SAMPLES, 0) == PTT_SAMPLES);
        events += tm_receive(&telemetry, decoded_pcm, PTT_SAMPLES, &event);
        if (atomic_load(&tx->microphone_ready)) break;
    }
    assert(atomic_load(&tx->microphone_ready));
    assert(mono_ms() - started >= 1400); /* 71 frames plus sink drain/guard */
    assert(events == 1 && !strcmp(event.id, "ALPHA"));
    set_ptt_state(tx, 0); wait_type(pair[1], PKT_END);
    atomic_store(&tx->running, 0);
    pthread_join(tx->send_thread, NULL); pthread_join(tx->play_thread, NULL);
    opus_decoder_destroy(wire_decoder);
    gst_element_set_state(tx->capture_pipeline, GST_STATE_NULL);
    gst_object_unref(tx->capture_sink); gst_object_unref(tx->capture_pipeline);
    gst_element_set_state(tx->playback_pipeline, GST_STATE_NULL);
    gst_object_unref(tx->playback_src); gst_object_unref(tx->playback_pipeline);
    ptt_jitter_destroy(&tx->jitter);
    pthread_mutex_destroy(&tx->jitter_lock); pthread_mutex_destroy(&tx->state_lock);

    /* Exercise the actual PCM playback pipeline with ALSA's null device. */
    rx->playback_pipeline = make_playback_pipeline(&rx->playback_src, "null");
    assert(rx->playback_pipeline);
    atomic_store(&rx->running, 1);
    assert(pthread_create(&rx->play_thread, NULL, play_thread_main, rx) == 0);
    msleep_int(180);
    atomic_store(&rx->suppress_playback, 1); msleep_int(60);
    atomic_store(&rx->suppress_playback, 0); msleep_int(60);
    atomic_store(&rx->running, 0); pthread_join(rx->play_thread, NULL);
    assert(atomic_load(&rx->rx_played_packets) >= 8);
    assert(atomic_load(&rx->playback_errors) == 0);
    gst_element_set_state(rx->playback_pipeline, GST_STATE_NULL);
    gst_object_unref(rx->playback_src); gst_object_unref(rx->playback_pipeline);
    ptt_jitter_destroy(&rx->jitter);
    pthread_mutex_destroy(&rx->jitter_lock); pthread_mutex_destroy(&rx->state_lock);
    close(pair[0]); close(pair[1]); free(tx); free(rx);
    puts("client encryption, receive and playback tests passed");
    return 0;
}
