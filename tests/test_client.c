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
    GstElement *capture = make_capture_pipeline(&capture_sink, "null", 1, 10);
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
