#!/usr/bin/env python3
"""Loopback integration checks for the actual UDP relay, with bounded waits."""
import os
import select
import signal
import socket
import struct
import subprocess
import time

HEADER = struct.Struct('!4sBBBBHHII16s32s24s')
assert HEADER.size == 92


def packet(kind=1, session=1, seq=0, encrypted=False):
    payload = b'\xf8\xff\xfe' if kind == 1 else b''
    # Relay intentionally does not decrypt: verify opaque encrypted forwarding.
    if encrypted:
        payload += b'x' * 16
    return HEADER.pack(b'UPTT', 2, kind, int(encrypted), 0, len(payload),
                       960 if kind == 1 else 0, seq, (seq * 960) & 0xffffffff,
                       session.to_bytes(16, 'big'), b'test', b'\0' * 24) + payload


def drain(sock):
    while select.select([sock], [], [], 0)[0]:
        sock.recv(4096)


def expect(sock, data, timeout=0.4):
    assert select.select([sock], [], [], timeout)[0], 'packet not forwarded'
    assert sock.recv(4096) == data


def absent(sock, timeout=0.04):
    assert not select.select([sock], [], [], timeout)[0], 'unexpected forwarded packet'


probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
probe.bind(('127.0.0.1', 0))
port = probe.getsockname()[1]
probe.close()
server = subprocess.Popen([os.environ.get('PTT_SERVER', './ptt_server'), '--port', str(port),
                           '--talker-hold-ms', '450', '--release-grace-ms', '100'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
clients = []
try:
    assert select.select([server.stdout], [], [], 2)[0], 'server did not start'
    line = server.stdout.readline()
    assert 'protocol=v2' in line, line
    for _ in range(3):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(('127.0.0.1', 0))
        sock.connect(('127.0.0.1', port))
        clients.append(sock)
        sock.send(packet(kind=0, session=0))
    a, b, listener = clients
    time.sleep(0.04)

    data = packet(session=1)
    a.send(data); expect(listener, data); expect(b, data); absent(a)
    b.send(packet(session=2)); absent(listener)
    # A 300 ms arrival gap remains within the configured 450 ms hold.
    time.sleep(0.30)
    data = packet(session=1, seq=1)
    a.send(data); expect(listener, data)
    b.send(packet(session=2, seq=1)); absent(listener)

    # END can arrive before the final audio and repeats must not extend grace.
    data = packet(kind=2, session=1, seq=3)
    a.send(data); expect(listener, data)
    tail = packet(session=1, seq=2)
    a.send(tail); expect(listener, tail)
    a.send(packet(session=1, seq=3)); absent(listener)
    time.sleep(0.08)
    a.send(data); absent(listener)  # retired session after grace
    next_data = packet(session=2, seq=2, encrypted=True)
    b.send(next_data); expect(listener, next_data)
    a.send(packet(kind=2, session=1, seq=3)); absent(listener)

    # New press from same sender supersedes old session. Old END cannot stop it.
    new = packet(session=3)
    b.send(new); expect(listener, new)
    b.send(packet(kind=2, session=2, seq=3)); absent(listener)
    new = packet(session=3, seq=1)
    b.send(new); expect(listener, new)
    # Timeout relinquishes ownership but permits resuming the same session.
    time.sleep(0.50)
    new = packet(session=3, seq=2)
    b.send(new); expect(listener, new)
    time.sleep(0.50)
    new = packet(session=4)
    a.send(new); expect(listener, new)
    b.send(packet(session=3, seq=3)); absent(listener)

    # Reject old wire format and malformed new packets rather than misdecoding.
    a.send(b'\x01' + b'\0' * 100); absent(listener)
    malformed = bytearray(packet(session=4, seq=1)); malformed[19] ^= 1
    a.send(malformed); absent(listener)
    # END before any audio preserves a short window for reordered audio.
    a.send(packet(kind=2, session=4, seq=1)); expect(listener, packet(kind=2, session=4, seq=1))
    time.sleep(0.15)
    early_end = packet(kind=2, session=5, seq=1)
    a.send(early_end); expect(listener, early_end)
    a.send(packet(session=5)); expect(listener, packet(session=5))
    print('server arbitration, reordering, timeout and protocol tests passed')
finally:
    for sock in clients:
        sock.close()
    server.send_signal(signal.SIGTERM)
    try:
        output, _ = server.communicate(timeout=3)
    except subprocess.TimeoutExpired:
        server.kill()
        output, _ = server.communicate()
        raise AssertionError('server failed to stop')
    assert server.returncode == 0, output
