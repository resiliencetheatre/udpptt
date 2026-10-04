#!/usr/bin/env python3
"""Actual gateway/relay round trips; no microphone or speaker required."""
import array
import math
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import wave

GATE = os.environ.get('PTT_WAV_GATE', './ptt_wav_gate')
SERVER = os.environ.get('PTT_SERVER', './ptt_server')


def wait_for(predicate, processes, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        assert all(p.poll() is None for p in processes), 'process exited unexpectedly'
        if predicate():
            return
        time.sleep(.03)
    raise AssertionError('timed out waiting for gateway')


def write_wav(path, seconds=.4, rate=24000, channels=1):
    samples = array.array('h', (int(12000 * math.sin(2 * math.pi * 440 * i / rate))
                               for i in range(int(rate * seconds)) for _ in range(channels)))
    if os.sys.byteorder != 'little':
        samples.byteswap()
    with wave.open(str(path), 'wb') as wav:
        wav.setparams((channels, 2, rate, 0, 'NONE', 'not compressed'))
        wav.writeframes(samples.tobytes())


def run_case(encrypted):
    with tempfile.TemporaryDirectory(prefix='ptt-wav-test-') as root:
        root = Path(root)
        processes, logs = [], []

        def start(args, name):
            log = open(root / (name + '.log'), 'w+')
            logs.append(log)
            p = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
            processes.append(p)
            return p

        def gate(name, extra=()):
            return [GATE, '127.0.0.1', '--port', str(port), '--input-dir', str(root / (name + '-in')),
                    '--output-dir', str(root / (name + '-out')), '--txid', name] + list(extra)

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        try:
            start([SERVER, '--port', str(port)], 'server')
            time.sleep(.1)
            crypto = ['--encrypt', '--key', 'test-word'] if encrypted else []
            receiver = start(gate('rx', crypto + ['--rx-only']), 'rx')
            sender = start(gate('tx', crypto + (['--preamble-id', 'ABCDE'] if encrypted else [])), 'tx')
            wait_for(lambda: (root / 'tx-in/sent').is_dir() and (root / 'rx-in/sent').is_dir(), processes)
            # Another queue owner must be rejected, and hardware flags rejected.
            duplicate = subprocess.run(gate('tx', crypto), capture_output=True, timeout=5)
            assert duplicate.returncode != 0 and b'lock input' in duplicate.stderr
            assert subprocess.run(gate('unused', ['--codec-ptt']), capture_output=True).returncode != 0
            if encrypted:
                start(gate('wrong', ['--rx-only', '--encrypt', '--key', 'wrong-word']), 'wrong')
            time.sleep(.7)
            queue, output = root / 'tx-in', root / 'rx-out'
            # Malformed/truncated WAVs stay queued, without blocking valid work.
            (queue / '00-invalid.wav').write_bytes(b'not a wave')
            write_wav(queue / '01-truncated.wav')
            data = (queue / '01-truncated.wav').read_bytes()
            (queue / '01-truncated.wav').write_bytes(data[:-100])
            # A hidden temporary file must not trigger TX.
            write_wav(queue / '.building.wav', channels=2)
            time.sleep(1.3)
            assert not list(output.glob('rx_*.wav'))
            (queue / '.building.wav').rename(queue / '03-ready.wav')
            wait_for(lambda: (queue / 'sent/03-ready.wav').exists() and len(list(output.glob('rx_*.wav'))) == 1, processes)
            recorded = next(output.glob('rx_*.wav'))
            with wave.open(str(recorded), 'rb') as wav:
                assert wav.getparams()[:3] == (1, 2, 48000)
                assert 19000 <= wav.getnframes() <= (500000 if encrypted else 21000), wav.getnframes()
                samples = array.array('h', wav.readframes(wav.getnframes()))
                assert max(abs(n) for n in samples) > 1000
            assert (queue / '00-invalid.wav').exists() and (queue / '01-truncated.wav').exists()
            assert not list(output.glob('*.part'))
            if encrypted:
                assert not list((root / 'wrong-out').glob('rx_*.wav'))
                assert '"id":"ABCDE"' in (root / 'rx.log').read_text()
            # A changed invalid file is reconsidered. Also covers non-20ms tails.
            write_wav(queue / '00-invalid.wav', seconds=.113, rate=16000)
            wait_for(lambda: (queue / 'sent/00-invalid.wav').exists() and len(list(output.glob('rx_*.wav'))) == 2, processes)
            # Back-to-back files must produce separate recordings, even though
            # the receiver still buffers the previous session when the next starts.
            write_wav(queue / '05-pair.wav', seconds=.13)
            write_wav(queue / '06-pair.wav', seconds=.13)
            wait_for(lambda: (queue / 'sent/06-pair.wav').exists() and len(list(output.glob('rx_*.wav'))) == 4, processes, timeout=20)
            # A longer send checks pacing and guards against accumulated timer
            # drift causing jitter-buffer underruns or extra concealed frames.
            if not encrypted:
                write_wav(queue / '07-paced.wav', seconds=3)
                before = set(output.glob('rx_*.wav'))
                wait_for(lambda: (queue / 'sent/07-paced.wav').exists() and len(list(output.glob('rx_*.wav'))) == 5, processes)
                recorded = (set(output.glob('rx_*.wav')) - before).pop()
                with wave.open(str(recorded), 'rb') as wav:
                    assert wav.getnframes() == 144000, wav.getnframes()
            # Repeated producer filenames must be sent and archived separately.
            # Preserve both the original and an existing suffixed archive.
            (queue / 'sent/speech.wav').write_bytes(b'preserve original')
            (queue / 'sent/speech_1.wav').write_bytes(b'preserve suffix')
            for suffix in (2, 3):
                before = len(list(output.glob('rx_*.wav')))
                write_wav(queue / '.speech.tmp', seconds=.15)
                original = (queue / '.speech.tmp').read_bytes()
                (queue / '.speech.tmp').rename(queue / 'speech.wav')
                archive = queue / f'sent/speech_{suffix}.wav'
                wait_for(lambda: archive.exists() and len(list(output.glob('rx_*.wav'))) == before + 1, processes)
                assert not (queue / 'speech.wav').exists()
                assert archive.read_bytes() == original
                assert (queue / 'sent/speech.wav').read_bytes() == b'preserve original'
                assert (queue / 'sent/speech_1.wav').read_bytes() == b'preserve suffix'
            # Receive-only mode must never consume its input files.
            write_wav(root / 'rx-in/ignored.wav')
            # Stop in mid-TX: source must remain queued, and partial RX finalized.
            write_wav(queue / '04-long.wav', seconds=4)
            wait_for(lambda: bool(list(output.glob('.*.part'))), processes)
            sender.terminate(); sender.wait(timeout=5)
            assert sender.returncode == 0
            processes.remove(sender)
            assert (queue / '04-long.wav').exists() and not (queue / 'sent/04-long.wav').exists()
            wait_for(lambda: len(list(output.glob('rx_*.wav'))) == (7 if encrypted else 8), processes)
            assert (root / 'rx-in/ignored.wav').exists()
            receiver.terminate(); receiver.wait(timeout=5)
            assert receiver.returncode == 0
            processes.remove(receiver)
        except Exception:
            for log in logs:
                log.flush(); log.seek(0)
                print('\n---', log.name, '---\n', log.read())
            raise
        finally:
            for p in processes:
                p.terminate()
            for p in processes:
                p.wait(timeout=5)
            for log in logs:
                log.close()


run_case(False)
run_case(True)
print('WAV gateway integration checks passed')
