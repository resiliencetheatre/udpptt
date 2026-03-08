# udpptt

Simple UDP push-to-talk audio test program using a client/server model.

Files:
  - ptt_client.c
  - ptt_server.c
  - Makefile

Build:
  make

Run server:
  ./ptt_server

Run client:
  ./ptt_client <server-ip> [--txid <callsign>] [--rx-only]
  ./ptt_client <server-ip> [--txid <callsign>] [--no-ptt]

Examples:
  ./ptt_client 198.51.100.10 --txid Alpha
  ./ptt_client 198.51.100.10 --txid Bravo --rx-only

Notes:
  - The client continuously sends UDP packets to the server from one local UDP socket.
  - Each packet contains a packet type and talk_id header.
  - When Right Alt (KEY_RIGHTALT / AltGr on many layouts) is pressed, the client sends microphone audio.
  - When the key is not pressed, the client sends idle frames to keep the return path alive.
  - The server listens on UDP/5000 and forwards only the active talker’s audio to other clients.
  - A sender never gets its own audio back from the server.
  - While the local client is transmitting, it suppresses playback of received audio.
  - In receive mode, incoming audio debug messages show the talk_id of the sending party.
  - The client scans readable /dev/input/event* devices for KEY_RIGHTALT. Membership in the input group is usually enough.
  - The --rx-only and --no-ptt options disable keyboard PTT handling and microphone capture, but still keep the client connected for receive/playback.

Packages typically needed on Debian/Ubuntu:

  sudo apt install build-essential pkg-config \
      libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
      gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
      gstreamer1.0-plugins-bad gstreamer1.0-tools

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
