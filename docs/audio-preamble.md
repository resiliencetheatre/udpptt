# Experimental audio telemetry preamble

Enable transmission with:

```sh
./ptt_client 198.51.100.10 --txid Alpha --preamble-id ALPHA
```

Each PTT press sends a new, audible data burst before microphone audio. The
same generated PCM is played locally instead of `start.wav`. Hold PTT, listen,
and speak when the distinct final cue ends. ID-only audio is 1.42 seconds;
GPS audio is 2.22 seconds (including 100 ms of on-air silence after the cue).
Actual readiness also depends on local sink startup/drain and scheduling.
`PTT READY` reports when microphone transmission opens.

Transmit is opt-in while the modem is experimental. Without `--preamble-id`,
legacy audio/start.wav behaviour remains. IDs contain exactly five ASCII
letters, normalized to uppercase. The in-band ID is independent of `--txid`,
which remains the transport/display name. Use matching names unless deliberately
identifying an audio gateway separately. Receive decoding is always enabled,
including `--rx-only`. No server or UDP header changes are needed.

Early release cancels the burst, closes its local monitor, and ends the session.
A new press always creates a fresh burst. If the local cue sink fails or fails
to drain within three seconds after the nominal burst duration, that PTT session
ends without opening the microphone; release and retry. `stop.wav` is unchanged.
The client yields the receive playback device to a finite, clocked cue pipeline
while transmitting; sink EOS, rather than appsrc queue completion, gates the
microphone. Microphone capture is discarded during the audible burst. After
sink EOS, the capture queue retains early speech during the remaining short
on-air silence. Playback device latency/echo behaviour still needs hardware
validation, especially with speakers, Bluetooth and exclusive ALSA devices.

## Optional live position

```sh
./ptt_client 198.51.100.10 --txid Alpha --preamble-id ALPHA \
    --gps-file "$XDG_RUNTIME_DIR/udpptt-gps.txt"
```

An external GPS producer writes one short text record:

```text
49.61160 6.13190 1791100000.000
```

Fields are latitude degrees, longitude degrees, and fix time as Unix seconds.
That example timestamp is illustrative; use the current fix time. Replace the
file atomically (write a temporary file, then rename it). The client snapshots
it on each press. Only regular files up to 256 bytes are accepted. Non-finite,
malformed, out-of-range, future, missing, and older-than-30-second fixes result
in an ID-only burst. `(0,0)` is a valid position. Fix age is rounded up to seconds.
Coordinates have 1e-5-degree resolution, not a guarantee of GPS accuracy.
No GPS daemon is required by the client; GPS acquisition belongs to the producer.

## Receive events and map boundary

A CRC-valid record appears on the terminal:

```text
TELEMETRY {"type":"udpptt.telemetry","version":1,"received_at_ms":1791100000000,"id":"ALPHA","sequence":42,"gps":true,"latitude":49.61160,"longitude":6.13190,"fix_age_s":3}
```

With `--telemetry-socket /path/to/listener.sock`, the same JSON object (without
`TELEMETRY `) is sent as a nonblocking Unix datagram to an existing listener.
The client does not create, delete or own that listener's path. Diagnostics stay
separate from machine-readable datagrams. ID-only events have `gps:false` and
omit coordinates and fix age. The sequence is a 16-bit per-process burst counter;
it wraps and is not a globally unique session identifier. Reception time is
local UTC Unix milliseconds at event publication, not the GPS measurement time.

A 16-entry queue decouples receive audio from terminal/socket output. Excess
events are dropped and counted in the `telemetry_drops` audio statistic. Each incoming
transport session has its own decoder before audio mixing. Repeated decoder
phase detections of the same burst are suppressed. Decoder search continues
through the stream, so an audio gateway may carry multiple bursts inside one
UDPPTT session. Local half-duplex suppression still applies to received sessions.

The burst remains in playback PCM: audio bridges can forward it. Arbitrarily
mixed/colliding audio cannot be separated by this modem. A bridge's PTT/VOX must
pass the acquisition and sync portions; the client does not control external
radio key-up or automatically open another system's voice gate. A node joining
a session after its preamble will hear speech without metadata.

The future map adapter should consume these datagrams and call
`python-front.py`'s `POST /api/positions`. Namespaced device identity and display
metadata belong in that adapter. The map currently requires heading, speed and
accuracy; this burst does not measure them. Support unknown values in the map
contract before posting, rather than fabricating zero measurements. ID-only
records must not create locations. This implementation does not modify the map
or send positions to it. For audio-only ingress, the offline decoder below is
available; a live sound-device listener is a follow-up integration.

CRC is corruption detection, not sender authentication. Audio bridges do not
preserve the UDP transport's encryption/authentication boundary.

## Experimental v1 waveform and wire format

This is an original experimental alphabet inspired by `Speech-Codebook.md`,
not a reconstruction of the referenced recording or proof of VoLTE operation.

- Mono PCM at 48 kHz; decoder averages groups of three samples to 16 kHz.
- 20 ms symbols, 16 pitches: `100 + 10*s` Hz, symbol `s=0..15`.
- Harmonics 2..12, each weighted by
  `exp(-((f-700)/450)^2) + 0.7*exp(-((f-1400)/600)^2)`.
  Their sum is scaled by `0.65/14`. Phase restarts for each symbol.
- Acquisition: ten alternating symbols `0,15`, starting with 0 (200 ms).
- Sync: `0,15,3,11,6,1,13,8,4,14,2,10,7,12,5,9` (320 ms).
- Four 20 ms frames of 1 kHz cue at amplitude 0.22, one frame of linear fade,
  followed by five frames of silence. The local monitor omits this final silence.
- Decoder evaluates harmonic energy at four timing phases spaced 5 ms apart.
  Sync accepts at most one symbol mismatch. This searches initial offset; it is
  **not** a continuous sample-rate/clock-drift tracking loop.

Payload bytes, in order (multibyte fields are big-endian):

| Bytes | Field |
|---|---|
| 1 | `0x10` ID-only or `0x11` with GPS; other values rejected |
| 3 | Five-letter base-26 ID, A=0, most significant letter first |
| 2 | Unsigned burst sequence |
| 4 optional | Signed two's-complement latitude, degrees × 100000 |
| 4 optional | Signed two's-complement longitude, degrees × 100000 |
| 2 optional | Unsigned fix age in seconds |
| 2 | CRC-16/CCITT-FALSE, poly 0x1021, init 0xffff, no reflection/xorout |

CRC covers the preceding bytes. Total length is 8 or 18 bytes. Feed bytes MSB
first to a rate-1/2, constraint-length-7 convolutional encoder, initial state
zero, generator order (171,133) octal. The register shifts left and inserts the
new bit at bit zero. Append six zero input bits to terminate. This produces
140 or 300 coded bits. For N coded bits, transmit symbol i as
`coded[i], coded[N/4+i], coded[2*N/4+i], coded[3*N/4+i]`, MSB first.
Symbols use direct binary numbering, not Gray coding.

The receiver tries the short payload after 35 data symbols, then the GPS
payload after 75 if necessary. Hard-decision Viterbi decoding ends in state
zero. Version/type, ID range, coordinate ranges and CRC must all validate.
An invalid/truncated frame emits nothing and never blocks speech. No ARQ or
automatic retransmission is used. Soft decisions, other alphabets and better
synchronization remain research work; any incompatible change needs a new
explicit waveform/profile identifier rather than silently changing v1.

## Reproduce experiments

```sh
make tools/telemetry_lab
python3 tools/telemetry_experiment.py /tmp/preamble-trial-new --count 24
```

The directory must not already exist. Each trial saves generated `tx.wav`,
post-Opus/PLC `rx.wav`, expected/received records, channel configuration and
metrics. The root contains the seed, waveform/FEC settings, source SHA-256s and
summary. It also saves full 16-symbol confusion matrices for raw PCM and Opus
over all ordered symbol pairs, along with their audio. Matrix column 16 denotes
silence/no decision; Opus windows are aligned using its reported lookahead. `rx.wav.packets` records every original Opus packet as little-endian
uint16 length, uint8 dropped flag, then encoded bytes. This is a laboratory
record format, not an Ogg or RTP file. The receiver uses PLC for dropped packets;
the simulation does not attempt Opus's in-band FEC recovery.

Individual commands:

```sh
tools/telemetry_lab encode ALPHA /tmp/alpha-new.wav 49.61160 6.13190
tools/telemetry_lab opus /tmp/alpha-new.wav /tmp/alpha-opus-new.wav 2 12345
tools/telemetry_lab decode /tmp/alpha-opus-new.wav
ffprobe -v error -show_streams -show_format /tmp/alpha-opus-new.wav
```

WAV outputs are exclusive-created and never overwrite earlier results. Decode
accepts 48 kHz, mono, PCM16 RIFF WAV, including extra RIFF chunks. Convert external
bridge recordings to that format first. `decode` exits 3 when no record passes
validation. The standalone decoder does not require a UDP packet header.

Initial 2026-10-04 experiment, 24 different records at each simulated loss rate:

| Opus packet loss | Correct bursts | Incorrect events |
|---|---:|---:|
| 0% | 24/24 | 0 |
| 2% | 23/24 | 0 |
| 5% | 20/24 | 0 |

The separate 512-symbol transition diagnostic measured 0 raw PCM symbol errors
and 13 post-Opus symbol errors before FEC (lookahead compensated). This is why
protected frame delivery, rather than raw symbol rate, is the useful metric.

These small deterministic trials are regression evidence, not a service-level
claim. `make test` also covers offsets, varied IDs, coordinate boundaries,
attenuation/noise, a lost payload frame, truncation, concurrent receive streams,
early PTT release and local cue completion. `make sanitize` runs ASan/UBSan.

Not yet validated: AMR-WB/AMR-NB/EVS, codec tandems, resampling and clock drift,
AGC/AEC/noise suppression/DTX, speaker-to-microphone coupling, real radio or
conference bridges, long-duration false-positive rate, and actual sound hardware
cue timing. The development environment has no AMR-WB encoder in ffmpeg. Run the
saved waveform through the intended channel before relying on position delivery.
