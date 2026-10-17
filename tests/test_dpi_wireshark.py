#!/usr/bin/env python3
"""Offline tests against a real TShark/plugin; no network or private key files.

Extra signed test cases use a fixed, publicly known test seed via ctypes/libsodium.
The checked-in fixture was independently constructed with cryptography/OpenSSL.
"""
import csv
import ctypes
import ctypes.util
import io
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import hashlib

ROOT = Path(__file__).resolve().parents[1]
TSHARK = os.environ.get('TSHARK', 'tshark')
PLUGIN = Path(os.environ.get('MDPX_PLUGIN', ROOT / 'wireshark/mdpx.so'))
PUBLIC = '03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8'
FIXTURE = bytes.fromhex((ROOT / 'tests/dpi_fixture.hex').read_text())
FIELDS = ['frame.number', 'mdpx.status', 'mdpx.structure_valid',
          'mdpx.capture_complete', 'mdpx.known_key', 'mdpx.authorized',
          'mdpx.signature_valid', 'mdpx.label', 'mdpx.payload_length',
          'mdpx.sequence', 'mdpx.timestamp_ms', 'mdpx.payload', '_ws.expert.message']


def run(args, env):
    child_env = env.copy()
    if child_env.get('MDPX_TEST_PRELOAD'):
        child_env['LD_PRELOAD'] = child_env['MDPX_TEST_PRELOAD']
    p = subprocess.run([TSHARK, *args], capture_output=True, text=True,
                       env=child_env, timeout=30)
    assert p.returncode == 0, (args, p.returncode, p.stderr)
    assert 'Dissector bug' not in p.stderr and 'assertion' not in p.stderr.lower(), p.stderr
    assert 'runtime error:' not in p.stderr and 'AddressSanitizer' not in p.stderr, p.stderr
    return p.stdout


def checksum(data):
    if len(data) % 2:
        data += b'\0'
    total = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return (~total) & 65535


def frame(payload, port=5200, ipv6=False):
    udp = struct.pack('!HHHH', 60000, port, len(payload) + 8, 0) + payload
    if ipv6:
        src = socket.inet_pton(socket.AF_INET6, '2001:db8::2')
        dst = socket.inet_pton(socket.AF_INET6, '2001:db8::1')
        pseudo = src + dst + struct.pack('!I3xB', len(udp), 17)
        udp = udp[:6] + struct.pack('!H', checksum(pseudo + udp) or 65535) + udp[8:]
        ip = struct.pack('!IHBB', 6 << 28, len(udp), 17, 64) + src + dst
        ethertype = 0x86dd
    else:
        ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, len(udp) + 20, 1,
                         0x4000, 64, 17, 0, b'\xc0\xa8\xb2\xa9',
                         b'\xc0\xa8\xb2\x29')
        ip = ip[:10] + struct.pack('!H', checksum(ip)) + ip[12:]
        ethertype = 0x0800
    return b'\x02\x02\x03\x04\x05\x06' * 2 + struct.pack('!H', ethertype) + ip + udp


def capture(path, records):
    with path.open('wb') as f:
        f.write(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 262144, 1))
        for wire, caplen in records:
            data = wire if caplen is None else wire[:caplen]
            f.write(struct.pack('<IIII', 1700000000, 0, len(data), len(wire)))
            f.write(data)


sodium = ctypes.CDLL(ctypes.util.find_library('sodium'))
sodium.sodium_init.restype = ctypes.c_int
assert sodium.sodium_init() >= 0
sodium.crypto_sign_seed_keypair.argtypes = [ctypes.c_void_p] * 3
sodium.crypto_sign_detached.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                      ctypes.c_void_p, ctypes.c_ulonglong,
                                      ctypes.c_void_p]
pk = ctypes.create_string_buffer(32)
sk = ctypes.create_string_buffer(64)
assert sodium.crypto_sign_seed_keypair(pk, sk, bytes(range(32))) == 0
assert pk.raw.hex() == PUBLIC


def signed(payload=b'', label=b'HOST-A', sequence=0, timestamp=0):
    header = (b'MDPX\x01\x00' + struct.pack('!HBH', 115 + len(label), len(label), len(payload))
              + hashlib.sha256(pk.raw).digest()[:8] + struct.pack('!Q', timestamp)
              + bytes(range(16)) + struct.pack('!Q', sequence) + label)
    message = b'metadata-proxy/v1' + header + payload
    sig = ctypes.create_string_buffer(64)
    assert sodium.crypto_sign_detached(sig, None, message, len(message), sk) == 0
    return header + sig.raw + payload


def mutated(offset, value=0x80):
    b = bytearray(FIXTURE)
    b[offset] ^= value
    return bytes(b)


with tempfile.TemporaryDirectory(prefix='mdpx-wireshark-') as tmp:
    d = Path(tmp)
    env = os.environ.copy()
    env['WIRESHARK_CONFIG_DIR'] = str(d / 'config')
    version = run(['--version'], env)
    match = re.search(r'\b(\d+\.\d+)\.\d+', version)
    assert match, version
    plugins = d / 'plugins'
    dest = plugins / match[1] / 'epan'
    dest.mkdir(parents=True)
    shutil.copy2(PLUGIN, dest / 'mdpx.so')
    env['WIRESHARK_PLUGIN_DIR'] = str(plugins)
    loaded = run(['-G', 'plugins'], env)
    assert str(dest / 'mdpx.so') in loaded, 'Test plugin not loaded: ' + loaded
    trust = d / 'trust.txt'
    trust.write_text(PUBLIC + ' HOST-A\n' + PUBLIC + ' ' + 'A' * 63 + '\n')
    wire = d / 'packets.pcap'

    def dissect(records, trust_file=trust, extra=(), display_filter=None):
        capture(wire, records)
        args = ['-o', 'mdpx.udp_port:5200', '-n', '-r', str(wire), '-T', 'fields', '-E', 'separator=\t']
        if trust_file is not None:
            args += ['-o', 'mdpx.trust_file:' + str(trust_file)]
        if display_filter:
            args += ['-Y', display_filter]
        args += list(extra)
        for field in FIELDS:
            args += ['-e', field]
        return [dict(zip(FIELDS, row)) for row in
                csv.reader(io.StringIO(run(args, env)), delimiter='\t')]

    def status(row, expected, signature=None):
        assert row['mdpx.status'] == str(expected), row
        assert row['mdpx.signature_valid'] == ('' if signature is None else str(signature)), row
        assert 'Malformed Packet' not in row['_ws.expert.message'], row

    cases = [
        (FIXTURE, 0, True),                       # Independent fixture, historic timestamp
        (mutated(124), 1, False),                 # Tampered payload
        (mutated(11), 2, None),                   # Unknown key ID
        (signed(b'x', b'HOST-B'), 3, True),       # Known key; unauthorized label
        (mutated(4), 4, None),                    # Version
        (mutated(5), 4, None),                    # Flags
        (FIXTURE + b'x', 4, None),                # Trailing bytes
        (FIXTURE[:-1], 4, None),                  # Short UDP payload, fully captured
        (signed(), 0, True),                      # Empty original payload
        (signed(bytes(range(256))), 0, True),
        (signed(b'X' * 1079), 0, True),
        (signed(b'Y' * (65507 - 178), b'A' * 63), 0, True),
        (signed(sequence=2**64 - 1, timestamp=2**64 - 1), 0, True),
        (FIXTURE, 0, True),                       # Repeated packet stays valid; no replay claim
    ]
    rows = dissect([(frame(b), None) for b, _, _ in cases])
    assert len(rows) == len(cases)
    for row, (_, result, valid) in zip(rows, cases):
        status(row, result, valid)
    assert rows[0]['mdpx.label'] == 'HOST-A'
    assert rows[0]['mdpx.payload'] == '00010203'
    assert rows[0]['mdpx.sequence'] == str(0x0102030405060708)
    assert rows[0]['mdpx.timestamp_ms'] == str(0x18bcfe56800)
    assert rows[3]['mdpx.authorized'] == 'False'
    assert rows[8]['mdpx.payload_length'] == '0'
    assert rows[12]['mdpx.sequence'] == str(2**64 - 1)
    assert rows[12]['mdpx.timestamp_ms'] == str(2**64 - 1)
    assert rows[-1]['mdpx.status'] == rows[0]['mdpx.status']

    # Every wire byte tampered: none may remain authenticated/authorized.
    rows = dissect([(frame(mutated(i)), None) for i in range(len(FIXTURE))])
    assert len(rows) == len(FIXTURE)
    assert all(row['mdpx.status'] not in ('', '0') for row in rows), rows
    # Every truncation point as a complete shorter datagram is malformed.
    rows = dissect([(frame(FIXTURE[:i]), None) for i in range(len(FIXTURE))])
    # Wireshark does not call subdissectors for an empty outer UDP payload.
    assert rows[0]['mdpx.status'] == ''
    for row in rows[1:]:
        status(row, 4)
    # Snaplen truncation must be unverified, not an invalid signature.
    full = frame(FIXTURE)
    rows = dissect([(full, 42 + i) for i in range(len(FIXTURE))])
    for row in rows:
        if row['frame.number'] == '1' and row['mdpx.status'] == '':
            continue  # No captured UDP payload to dispatch.
        status(row, 5)
        assert row['mdpx.capture_complete'] == 'False'
        assert row['mdpx.structure_valid'] == ''
    # Both IP versions, including capture truncation after a complete UDP header.
    rows = dissect([(frame(FIXTURE, ipv6=True), None), (frame(FIXTURE, ipv6=True), 70)])
    status(rows[0], 0, True)
    status(rows[1], 5)

    base = [(frame(FIXTURE), None)]
    status(dissect(base, trust_file=None)[0], 6)
    empty = d / 'empty'; empty.write_text('')
    status(dissect(base, trust_file=empty)[0], 2)
    missing = d / 'missing'
    status(dissect(base, trust_file=missing)[0], 7)
    bad = d / 'bad'
    for contents in [PUBLIC + ' HOST-A\nbad\n', PUBLIC + ' HOST-A\0hidden\n', 'x'*1000]:
        bad.write_text(contents)
        status(dissect(base, trust_file=bad)[0], 7)
    other = d / 'other'; other.write_text(PUBLIC + ' OTHER\n')
    status(dissect(base, trust_file=other)[0], 3, True)
    # Label authorization never substitutes for signature validity.
    status(dissect([(frame(mutated(124)), None)], trust_file=other)[0], 1, False)
    both = d / 'both'; both.write_text(trust.read_text() + PUBLIC + ' HOST-B\n')
    status(dissect([(frame(signed(b'x', b'HOST-B')), None)], trust_file=both)[0], 0, True)

    # Configurable port, disabling automatic binding, and Decode As.
    alt = [(frame(FIXTURE, port=6200), None)]
    assert dissect(alt)[0]['mdpx.status'] == ''
    status(dissect(alt, extra=['-o', 'mdpx.udp_port:6200'])[0], 0, True)
    assert dissect(base, extra=['-o', 'mdpx.udp_port:0'])[0]['mdpx.status'] == ''
    status(dissect(alt, extra=['-o', 'mdpx.udp_port:0', '-d', 'udp.port==6200,mdpx'])[0], 0, True)

    ethernet = b'\x02\x02\x03\x04\x05\x06' * 2 + struct.pack('!H', 0x88b5) + FIXTURE
    status(dissect([(ethernet, None)], extra=['-d', 'ethertype==0x88b5,mdpx'])[0], 0, True)

    selected = [(frame(FIXTURE), None), (frame(mutated(124)), None), (frame(mutated(11)), None)]
    rows = dissect(selected, display_filter='mdpx.signature_valid == true && mdpx.authorized == true')
    assert len(rows) == 1 and rows[0]['frame.number'] == '1', rows
    rows = dissect(selected, display_filter='mdpx.signature_valid == false')
    assert len(rows) == 1 and rows[0]['frame.number'] == '2', rows
    # Two-pass dissection must not change results or treat redissection as replay.
    single = dissect(selected)
    double = dissect(selected, extra=['-2'])
    assert single == double, (single, double)
    print('Wireshark', match[1], ': plugin loading, signatures, trust, fields, all-byte tampering,')
    print('all truncation points, IPv4/IPv6, empty/maximum payloads, Decode As, filters, and two-pass tests passed')
