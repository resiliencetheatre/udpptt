#!/usr/bin/env python3
"""Local PTY integration test: no models, audio devices or network needed."""
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import tempfile
import termios
import time
import wave

ROOT = Path(__file__).resolve().parent


def until(predicate, pump, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        pump()
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError('Timed out waiting for integration result')


with tempfile.TemporaryDirectory(prefix='operator-test-') as tmp:
    base = Path(tmp)
    incoming, outgoing = base / 'in', base / 'out'
    incoming.mkdir()
    outgoing.mkdir()
    helper = base / 'kokoro'
    helper.write_text('''#!/usr/bin/env python3
import json, os, sys, time, wave
from pathlib import Path
args = sys.argv[1:]
text = args[args.index('--text') + 1]
with open(os.environ['CALLS'], 'a') as log: log.write(json.dumps(args) + '\\n')
if text == 'FAIL': sys.exit(1)
if text == 'SLOW':
    Path(os.environ['SLOW_PID']).write_text(str(os.getpid()))
    time.sleep(60)
target = args[args.index('--output-file') + 1]
with wave.open(target, 'wb') as wav:
    wav.setparams((1, 2, 24000, 0, 'NONE', 'not compressed'))
    wav.writeframes(b'\\0\\0' * 2400)
''')
    helper.chmod(0o755)
    whisper = base / 'whisper'
    whisper.write_text('#!/bin/sh\necho "Received test message"\n')
    whisper.chmod(0o755)
    model = base / 'model'
    model.touch()
    fakegate = base / 'gate'
    fakegate.write_text('''#!/usr/bin/env python3
import json, os, sys, time
from pathlib import Path
Path(os.environ['GATE_ARGS']).write_text(json.dumps(sys.argv[1:]))
time.sleep(60)
''')
    fakegate.chmod(0o755)
    config = base / 'test.env'
    config.write_text(f"""# Isolated integration-test settings
OPERATOR_SERVER=192.0.2.1
OPERATOR_TXID=TestOperator
OPERATOR_KEY=test secret #literal
OPERATOR_PREAMBLE_ID=TEST
OPERATOR_INPUT_DIR={incoming}
OPERATOR_OUTPUT_DIR={outgoing}
OPERATOR_STATE_FILE={base / 'state'}
OPERATOR_GATE={fakegate}
OPERATOR_KOKORO={helper}
OPERATOR_VOICE=file-voice
WHISPER_BIN={whisper}
WHISPER_MODEL={model}
OPERATOR_JITTER_MS=150
OPERATOR_FEC_LOSS_PERCENT=5
OPERATOR_RX_STATE_TIMEOUT_MS=750
""")
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('OPERATOR_', 'WHISPER_'))}
    env.update(TERM='xterm-256color', LC_ALL='C.UTF-8',
               OPERATOR_TXID='EnvironmentOperator', OPERATOR_VOICE='environment-voice',
               CALLS=str(base / 'calls'), SLOW_PID=str(base / 'slow-pid'),
               GATE_ARGS=str(base / 'gate-args'))
    for key in ('STATE_FILE', 'ERROR_LOG'):
        env.pop(key, None)
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 100, 0, 0))
    args = [str(ROOT / 'operator'), '--env-file', str(config),
            '--start-gate', '--voice', 'af_heart']
    proc = subprocess.Popen(args, stdin=slave, stdout=slave, stderr=slave, env=env)
    os.close(slave)
    screen = bytearray()

    def pump():
        while select.select([master], [], [], 0)[0]:
            try:
                data = os.read(master, 65536)
            except OSError:
                break
            if not data:
                break
            screen.extend(data)

    try:
        until(lambda: b'Enter: send' in screen, pump)
        until(lambda: (base / 'gate-args').exists(), pump)
        gate_args = json.loads((base / 'gate-args').read_text())
        assert gate_args[0] == '192.0.2.1'
        for flag, value in [('--txid', 'EnvironmentOperator'), ('--key', 'test secret #literal'),
                            ('--preamble-id', 'TEST'), ('--jitter-ms', '150'),
                            ('--fec-loss-percent', '5'), ('--rx-state-timeout-ms', '750')]:
            assert gate_args[gate_args.index(flag) + 1] == value
        message = 'Negative; $(touch SHOULD_NOT_EXIST) café'
        os.write(master, (message + '\nSecond\n').encode())
        until(lambda: len(list(incoming.glob('*.wav'))) == 2, pump)
        calls = [json.loads(line) for line in (base / 'calls').read_text().splitlines()]
        assert calls[0][calls[0].index('--text') + 1] == message
        assert calls[1][calls[1].index('--text') + 1] == 'Second'
        assert calls[0][calls[0].index('--voice') + 1] == 'af_heart'
        assert not list(incoming.glob('.operator-*'))
        os.write(master, b'FAIL\n')
        until(lambda: b'Kokoro failed' in screen, pump)
        assert len(list(incoming.glob('*.wav'))) == 2
        with wave.open(str(outgoing / 'rx.wav'), 'wb') as wav:
            wav.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
            wav.writeframes(b'\0\0' * 4800)
        until(lambda: (outgoing / 'archive' / 'rx.wav').exists(), pump)
        until(lambda: b'Received test message' in screen, pump)
        assert 'Received test message' in (outgoing / 'operator' / 'transcriptions.log').read_text()
        # Exclusive ownership protects the watcher and TTS queues.
        duplicate = subprocess.run(args, env=env, capture_output=True, timeout=3)
        assert duplicate.returncode != 0
        assert b'already be running' in duplicate.stderr
        # Resize, history navigation, editing, then terminate during active synthesis.
        fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack('HHHH', 12, 45, 0, 0))
        proc.send_signal(signal.SIGWINCH)
        os.write(master, b'\x1b[5~\x1b[6~junk\x15SLOW\n')
        until(lambda: (base / 'slow-pid').exists(), pump)
        slow_pid = int((base / 'slow-pid').read_text())
        os.write(master, b'\x11')
        until(lambda: proc.poll() is not None, pump)
        assert proc.returncode == 0, bytes(screen[-6000:]).decode(errors="replace")
        assert not list(incoming.glob('.operator-*'))
        try:
            os.kill(slow_pid, 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError('Kokoro survived operator shutdown')
    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=5)
        os.close(master)
    invalid = base / 'invalid.env'
    invalid.write_text('export OPERATOR_KEY=do-not-print-this-value\n')
    for command in ([str(ROOT / 'operator')], ['bash', str(ROOT / 'watch-whisper.sh')]):
        result = subprocess.run(command, env=dict(env, OPERATOR_ENV_FILE=str(invalid)),
                                capture_output=True, timeout=3)
        assert result.returncode == 2
        assert b'Invalid environment entry at line 1' in result.stderr
        assert b'do-not-print-this-value' not in result.stderr
    empty = base / 'empty.env'
    empty.touch()
    result = subprocess.run([str(ROOT / 'operator'), '--env-file', str(empty)],
                            env=env, capture_output=True, timeout=3)
    assert result.returncode == 2
    assert b'Set OPERATOR_INPUT_DIR' in result.stderr
print('operator PTY integration: PASS')
