# udpptt

![Intro picture](img/udpptt.png "udpptt diagram")

Simple UDP push-to-talk audio test program using a client/server model.


## UDP connection behavior

The client keeps one UDP path continuously active by sending regular 
idle packets to the server even when no one is talking. This means the 
server always sees fresh outbound traffic from the client and can 
immediately send return audio back along the same already-active UDP 
mapping when a PTT stream becomes available. In practice, this avoids 
the need for the client to wait for a new inbound path to be 
established at the moment speech starts. A useful side effect is that 
clients usually do not need any manually opened inbound firewall or 
NAT port-forwarding rules, because the outbound UDP traffic keeps the 
return path alive for server-to-client audio delivery. This makes the 
setup much simpler for roaming laptops, home networks, and small 
embedded devices behind typical consumer routers or local firewalls.

Packages typically needed on Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config libsodium-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad gstreamer1.0-tools
```

## Installation with `make install`

Build and install the binaries, tone files, and user service with:

```sh
make
sudo make install
```

The install step does the following:

- installs `ptt_client` and `ptt_server` to `/usr/local/bin`
- installs `start.wav` and `stop.wav` to `/opt/udpptt`
- installs a user systemd service file to:

```text
~/.config/systemd/user/udpptt.service
```

- installs a per-user configuration file to:

```text
~/.config/udpptt/udpptt.env
```

The service is configured to run with:

```text
WorkingDirectory=/opt/udpptt
```

This means `ptt_client` will look for `start.wav` and `stop.wav` from `/opt/udpptt` when started through systemd.

After installation, enable the user service:

```sh
systemctl --user daemon-reload
systemctl --user enable --now udpptt.service
```

To view logs:

```sh
journalctl --user -u udpptt.service -f
```

## User configuration file

The runtime parameters are read from:

```text
~/.config/udpptt/udpptt.env
```

Example:

```sh
SERVER_IP=198.51.100.10
CALL_SIGN=Alpha
WORD_OF_DAY=shared-room-secret
ALTGR_PTT_DELAY_MS=2000
```

This file is intended to be edited by the user after installation. It defines the server address, callsign, encryption password, and AltGr PTT hold delay.

## Manual installation steps and details

Build:

```sh
make
```

Run server:

```sh
./ptt_server
```

Run client:

```sh
./ptt_client <server-ip> [--txid <callsign>] [--rx-only]
./ptt_client <server-ip> [--txid <callsign>] [--no-ptt]
./ptt_client <server-ip> [--txid <callsign>] [--encrypt] [--key <password>]
UDPPTT_KEY='<password>' ./ptt_client <server-ip> [--txid <callsign>] [--encrypt]

./ptt_client <server-ip> [--codec-ptt]
./ptt_client <server-ip> [--altgr-ptt-delay-ms <ms>]

./ptt_client <server-ip> [--rpi-audio]
./ptt_client <server-ip> [--alsa-device <device>]
./ptt_client <server-ip> [--alsa-capture-device <device>] [--alsa-playback-device <device>]
```

Examples:

```sh
./ptt_client 198.51.100.10 --txid Alpha
./ptt_client 198.51.100.10 --txid Bravo --rx-only
./ptt_client 198.51.100.10 --txid Alpha --encrypt --key 'shared room secret'
UDPPTT_KEY='shared room secret' ./ptt_client 198.51.100.10 --txid Bravo --rx-only --encrypt

./ptt_client 198.51.100.10 --txid Alpha --altgr-ptt-delay-ms 2000
./ptt_client 198.51.100.10 --txid Alpha --altgr-ptt-delay-ms 700 --encrypt --key 'shared room secret'

./ptt_client 198.51.100.10 --txid Bravo --codec-ptt --rpi-audio --encrypt --key 'shared room secret'
./ptt_client 198.51.100.10 --txid Bravo --alsa-device plughw:0,0 --codec-ptt
./ptt_client 198.51.100.10 --txid Bravo --alsa-capture-device plughw:0,0 --alsa-playback-device plughw:0,0
```

## Notes

- The client continuously sends UDP packets to the server from one local UDP socket.
- Each packet contains a packet type and cleartext `talk_id` header.
- When encryption is enabled, the Opus payload is encrypted end-to-end between clients.
- The `talk_id` remains visible for logging/debug, but is authenticated together with the packet.
- The server does not encrypt or decrypt payloads; it only forwards packets.
- Encrypted and unencrypted clients do not interoperate on the same channel.
- All encrypted clients must use the same shared password.
- The server listens on UDP/5000 and forwards only the active talker’s audio to other clients.
- A sender never gets its own audio back from the server.
- While the local client is transmitting, it suppresses playback of received audio.
- In receive mode, incoming audio debug messages show the `talk_id` of the sending party.
- The `--rx-only` and `--no-ptt` options disable keyboard PTT handling and microphone capture, but still keep the client connected for receive/playback.
- The client can optionally play local `start.wav` and `stop.wav` tones on PTT press/release if those files exist in the current working directory.

## PTT input modes

### PC keyboard mode

- By default, the client uses **Right Alt / AltGr** as the PTT key on normal PC hardware.
- AltGr PTT activation is delayed by a configurable hold timer.
- Default AltGr hold delay is **2000 ms**.
- This allows AltGr to still be used for normal typing, while a long press activates PTT.
- You can change the delay with:

```sh
./ptt_client <server-ip> --altgr-ptt-delay-ms 2000
```

### Codec / embedded PTT mode

- The `--codec-ptt` option switches PTT input handling to **KEY_ENTER**.
- This is intended for embedded or codec-board GPIO/button input devices such as `ptt_keys`.
- In `--codec-ptt` mode, PTT activation is immediate and does not use the AltGr delay logic.

## Audio device selection

### Default desktop mode

- If no ALSA device options are given, the client uses:
  - `autoaudiosrc` for capture
  - `autoaudiosink` for playback

This is convenient on normal desktop Linux systems.

### Raspberry Pi / explicit ALSA mode

- The client can use explicit ALSA devices for both capture and playback.
- `--rpi-audio` is a convenience option that sets both capture and playback to:

```text
plughw:0,0
```

- You can also configure them manually:
  - `--alsa-device <dev>` sets both capture and playback
  - `--alsa-capture-device <dev>` sets only capture
  - `--alsa-playback-device <dev>` sets only playback

Examples:

```sh
./ptt_client 198.51.100.10 --rpi-audio --codec-ptt --txid Bravo
./ptt_client 198.51.100.10 --alsa-device plughw:0,0 --txid Bravo
./ptt_client 198.51.100.10 --alsa-capture-device plughw:1,0 --alsa-playback-device plughw:0,0
```

This is useful on Buildroot and Raspberry Pi systems where automatic GStreamer audio sink/source selection may not work reliably.

## Permissions

- The client scans readable `/dev/input/event*` devices for the selected PTT key.
- Membership in the `input` group is usually enough.
- On embedded targets using `--codec-ptt`, make sure the `ptt_keys` input device is accessible to the user running `ptt_client`.

## systemd service

```
[Unit]
Description=udpptt client
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
EnvironmentFile=%h/.config/udpptt/udpptt.env
ExecStart=/usr/local/bin/ptt_client ${SERVER_IP} --altgr-ptt-delay-ms ${ALTGR_PTT_DELAY_MS} --txid ${CALL_SIGN} --encrypt --key ${WORD_OF_DAY}
Restart=always
RestartSec=3

[Install]
WantedBy=default.target
```

Suggested paths for service file: 

```
~/.config/systemd/user/udpptt.service
```

udpptt.env path and content:

```
~/.config/udpptt/udpptt.env
```

```
SERVER_IP=198.51.100.10
CALL_SIGN=Alpha
WORD_OF_DAY=shared-room-secret
ALTGR_PTT_DELAY_MS=2000
```

Install service file

```
mkdir -p ~/.config/systemd/user
nano ~/.config/systemd/user/udpptt.service
systemctl --user daemon-reload
systemctl --user enable --now udpptt.service
systemctl --user status udpptt.service
```

If you want the client to keep running even when the user is not logged in, enable lingering for that user:

```
sudo loginctl enable-linger USERNAME
```

Because ptt_client reads /dev/input/event*, the user running the 
service must have permission to access those input devices. 
On Debian, a common approach is to add that user to the input group:

```
sudo usermod -aG input $USER
```


License
=======

This project is licensed under the GNU General Public License,
either version 3 of the License, or (at your option) any later version.

Copyright (C) 2026 Resilience Theatre

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this project. If not, see <https://www.gnu.org/licenses/>.
