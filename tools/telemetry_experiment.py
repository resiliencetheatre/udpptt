#!/usr/bin/env python3
"""Save deterministic waveform/Opus experiments in a fresh directory."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("directory", type=Path)
parser.add_argument("--count", type=int, default=24)
args = parser.parse_args()
if not 1 <= args.count <= 1000:
    parser.error("count must be 1..1000")
root = Path(__file__).resolve().parent.parent
lab = root / "tools/telemetry_lab"
args.directory.mkdir(parents=True, exist_ok=False)
rng = random.Random(20261004)
config = {
    "seed": 20261004, "count_per_loss_rate": args.count,
    "loss_percent": [0, 2, 5], "sample_rate": 48000,
    "symbol_ms": 20, "pitches_hz": list(range(100, 251, 10)),
    "codec": "libopus VOIP, 24000 bps, FEC on, expected loss 10%; receiver PLC",
    "waveform": "harmonics 2..12, Gaussian envelopes at 700 and 1400 Hz",
    "fec": "K7 rate 1/2 (171,133 octal), hard Viterbi, four-row interleave",
    "sources_sha256": {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                       for name in ["ptt_telemetry.c", "ptt_telemetry.h", "tools/telemetry_lab.c"]},
}
(args.directory / "config.json").write_text(json.dumps(config, indent=2) + "\n")

def run(*command):
    return subprocess.run([str(lab), *map(str, command)], capture_output=True, text=True, check=True)

confusion = json.loads(run("confusion", args.directory / "confusion").stdout)
(args.directory / "confusion.json").write_text(json.dumps(confusion, indent=2) + "\n")
summary = []
for loss in config["loss_percent"]:
    good = false = received = 0
    for i in range(args.count):
        directory = args.directory / f"loss-{loss:02d}-{i:04d}"
        directory.mkdir()
        ident = "".join(rng.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ") for _ in range(5))
        gps = i % 2 == 1
        coords = [f"{rng.uniform(-80, 80):.5f}", f"{rng.uniform(-180, 180):.5f}"] if gps else []
        tx, rx = directory / "tx.wav", directory / "rx.wav"
        synthesis = json.loads(run("encode", ident, tx, *coords).stdout)
        channel = json.loads(run("opus", tx, rx, loss, i + 1).stdout)
        decoded = subprocess.run([str(lab), "decode", str(rx)], capture_output=True, text=True)
        if decoded.returncode not in (0, 3):
            raise RuntimeError(decoded.stderr)
        events = [json.loads(line) for line in decoded.stdout.splitlines()]
        expected = {"id": ident, "sequence": 1, "gps": gps}
        if gps:
            expected.update(latitude=float(coords[0]), longitude=float(coords[1]), fix_age_s=0)
        matches = [all(event.get(key) == value for key, value in expected.items()) for event in events]
        good += len(events) == 1 and all(matches)
        false += sum(not match for match in matches)
        received += len(events)
        report = dict(expected=expected, synthesis=synthesis, channel=channel, received=events)
        (directory / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    summary.append(dict(loss_percent=loss, trials=args.count, correct_bursts=good,
                        received_events=received, incorrect_events=false))
(args.directory / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary, indent=2))
