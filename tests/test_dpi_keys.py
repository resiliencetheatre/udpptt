#!/usr/bin/env python3
"""Signing-key lifecycle and CLI validation; no audio devices or network needed."""
from pathlib import Path
import stat
import subprocess
import tempfile


def run(args):
    return subprocess.run(args, capture_output=True, timeout=5)


with tempfile.TemporaryDirectory(prefix='udpptt-dpi-keys-') as temp:
    root = Path(temp)
    secret, public = root / 'sender.secret', root / 'sender.public'
    args = ['./ptt_dpi_keygen', '--secret', str(secret), '--public', str(public)]
    assert run(args).returncode == 0
    seed, key = secret.read_bytes(), public.read_bytes()
    assert len(seed) == len(key) == 65 and seed[-1:] == key[-1:] == b'\n'
    assert len(bytes.fromhex(seed.decode())) == len(bytes.fromhex(key.decode())) == 32
    assert stat.S_IMODE(secret.stat().st_mode) == 0o600
    assert run(args).returncode != 0
    assert secret.read_bytes() == seed and public.read_bytes() == key
    for label in ['', 'a' * 64, 'has space', 'has/slash', 'has\nnewline', 'ä']:
        result = run(['./ptt_client', '127.0.0.1', '--dpi-id', label])
        assert result.returncode != 0 and b'--dpi-id requires' in result.stderr, result
    for contents in [b'', b'0' * 64, b'g' * 64 + b'\n', seed + b'\n', b'0' * 63 + b'\0\n']:
        bad = root / 'bad.secret'
        bad.write_bytes(contents)
        result = run(['./ptt_client', '127.0.0.1', '--dpi-id', 'HOST-A', '--dpi-secret', str(bad)])
        assert result.returncode != 0 and b'Cannot load DPI signing seed' in result.stderr, result
    result = run(['./ptt_client', '127.0.0.1', '--dpi-secret', str(secret)])
    assert result.returncode != 0 and b'--dpi-secret requires --dpi-id' in result.stderr
print('DPI key creation, permissions, no-overwrite and CLI validation passed')
