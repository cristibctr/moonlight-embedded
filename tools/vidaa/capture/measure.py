#!/usr/bin/env python3
"""Sample the actual TV video/OSD capture, with clock bounds and frame markers.

This measures source-draw to TV-capture delay, NOT optical panel or input latency.
Capture is sparse. A low-rate recording cannot establish the TV's 60 Hz cadence.
"""
import argparse
import binascii
import csv
import json
import math
import os
from pathlib import Path
import select
import statistics
import subprocess
import time

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent


def decode_marker_info(raw, width, height):
    pixels = np.frombuffer(raw, dtype=np.uint8).reshape(height, width, 4)
    # Warm color processing can turn white into amber. Use brightness.
    white = np.mean(pixels[:, :, :3], axis=2) > 180
    # Long horizontal border runs exclude the pointer and TV caption text.
    runs = []
    for y in range(height):
        edges = np.diff(np.pad(white[y].astype(np.int8), (1, 1)))
        for left, right in zip(np.where(edges == 1)[0], np.where(edges == -1)[0]):
            if right-left > width*.4:
                runs.append((int(left), int(right), y))
    if not runs:
        raise ValueError("no white marker border")
    longest = max(r-l for l,r,y in runs)
    border = [(l,r,y) for l,r,y in runs if r-l >= longest-3]
    left = int(statistics.median(l for l,r,y in border))
    right = int(statistics.median(r for l,r,y in border))
    top, bottom = min(y for l,r,y in border), max(y for l,r,y in border)+1
    if right-left < width * .4 or bottom-top < height * .25:
        raise ValueError("marker is too small")
    for columns, magic in ((10, b'ML'), (14, b'MN')):
        bits = []
        for bit in range(columns * 8):
            x = left + (right-left) * (.05 + .9 * ((bit % columns + .5) / columns))
            y = top + (bottom-top) * (.05 + .9 * ((bit // columns + .5) / 8))
            patch = pixels[int(y)-1:int(y)+2, int(x)-1:int(x)+2, :3]
            bits.append(int(float(patch.mean()) > 128))
        payload = bytes(sum(bits[i+j] << (7-j) for j in range(8))
                        for i in range(0, columns * 8, 8))
        if payload[:2] != magic:
            continue
        if binascii.crc_hqx(payload[:-2], 0xffff) != int.from_bytes(payload[-2:], 'big'):
            raise ValueError('marker checksum mismatch (possible mixed frame)')
        return (int.from_bytes(payload[2:6], 'big'), int.from_bytes(payload[6:8], 'big'),
                int.from_bytes(payload[8:12], 'big') if columns == 14 else None)
    raise ValueError('marker magic mismatch')


def decode_marker(raw, width, height):
    return decode_marker_info(raw, width, height)[:2]


class Capture:
    def __init__(self, output, width, height, capture_type, inner_timing=False):
        self.log = (output / "tv-capture.log").open("wb")
        self.process = subprocess.Popen([
            "ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "vidaa-tv",
            f"/tmp/run-moonlight-capture {'--inner-timing ' if inner_timing else ''}{capture_type} {width} {height} -"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log, bufsize=0)
        self.buffer = bytearray()
        try:
            if self.line() != "READY 1":
                raise RuntimeError("TV capture did not start; see tv-capture.log")
        except Exception:
            self.close()
            raise

    def read(self, size=None):
        deadline = time.monotonic() + 20
        while (len(self.buffer) < size if size is not None else b"\n" not in self.buffer):
            remaining = deadline-time.monotonic()
            if remaining <= 0 or not select.select([self.process.stdout], [], [], remaining)[0]:
                raise TimeoutError("TV capture response timeout")
            block = os.read(self.process.stdout.fileno(), 1024*1024)
            if not block:
                raise RuntimeError("TV capture closed; see tv-capture.log")
            self.buffer.extend(block)
        if size is None:
            size = self.buffer.index(b"\n") + 1
        data = bytes(self.buffer[:size])
        del self.buffer[:size]
        return data

    def line(self):
        return self.read().decode("ascii").strip()

    def command(self, command):
        self.process.stdin.write((command + "\n").encode("ascii"))
        self.process.stdin.flush()

    def ping(self, count):
        samples = []
        for i in range(count):
            sent = time.monotonic_ns() / 1000
            self.command(f"PING {i}")
            reply = self.line().split()
            received = time.monotonic_ns() / 1000
            if reply[:2] != ["PONG", str(i)]:
                raise RuntimeError("invalid clock reply")
            tv_received, tv_sent = map(int, reply[2:])
            samples.append({"host_send_us": sent, "host_receive_us": received,
                            "tv_receive_us": tv_received, "tv_send_us": tv_sent,
                            "offset_low_us": tv_sent-received,
                            "offset_high_us": tv_received-sent})
        return samples

    def frame(self, index):
        self.command(f"CAP {index}")
        header = self.line().split()
        if header[:2] != ["FRAME", str(index)] or len(header) != 7:
            raise RuntimeError(f"bad frame header: {header}")
        start, end, width, height, length = map(int, header[2:])
        if not (64 <= width <= 1920 and 64 <= height <= 1080 and length == width*height*4):
            raise RuntimeError("bad frame length")
        return start, end, width, height, self.read(length)

    def close(self):
        try:
            if self.process.poll() is None:
                self.command("QUIT")
                self.process.wait(timeout=5)
        except (BrokenPipeError, subprocess.TimeoutExpired):
            self.process.terminate()  # Only the SSH process started here.
        self.log.close()


def summarize(rows, offsets, source_uncertainty_ms=0, source_label='Mac', source_clock='mac'):
    if not math.isfinite(source_uncertainty_ms) or source_uncertainty_ms < 0:
        raise ValueError('Invalid source clock uncertainty')
    # Intersection is valid for a constant clock offset during this short run.
    if source_clock not in ('mac', 'tv'):
        raise ValueError('Unknown source clock domain')
    # A TV-calibrated source uses the capture/trace clock directly. Mac sleep
    # cannot alter this subtraction; source calibration still needs validation.
    low = max(p["offset_low_us"] for p in offsets) if source_clock == 'mac' else 0
    high = min(p["offset_high_us"] for p in offsets) if source_clock == 'mac' else 0
    if low > high:
        raise RuntimeError("Clock bounds conflict; shorten test or account for clock drift")
    for row in rows:
        row["capture_ms"] = (row["tv_end_us"]-row["tv_start_us"]) / 1000
        if "source_ms" in row:
            row["delay_low_ms"] = (row["tv_start_us"]-high) / 1000 - (row["source_ms"]+1) - source_uncertainty_ms
            row["delay_high_ms"] = (row["tv_end_us"]-low) / 1000 - row["source_ms"] + source_uncertainty_ms
            row["delay_mid_ms"] = (row["delay_low_ms"]+row["delay_high_ms"]) / 2
    valid = [r for r in rows if "source_ms" in r]
    report = {
        "metric": f"{source_label} marker draw to TV capture (not optical panel latency)",
        "source_label": source_label,
        "source_clock": source_clock,
        "source_uncertainty_ms": source_uncertainty_ms,
        "samples": len(rows), "valid_markers": len(valid),
        "clock_offset_low_us": low, "clock_offset_high_us": high,
        "clock_uncertainty_ms": (high-low) / 1000,
        "capture_duration_median_ms": statistics.median(r["capture_ms"] for r in rows),
        "capture_duration_max_ms": max(r["capture_ms"] for r in rows),
        "limits": ["Capture time is bounded by API call start/end, not a hardware exposure timestamp.",
                   "Capture may change system load; measure at a low rate.",
                   "No optical panel delay, input/controller delay, or sound delay is measured.",
                   "Sparse samples detect long stalls but cannot prove 60 displayed frames per second.",
                   "The marker timestamp is a draw timestamp, not a display fence."]}
    if valid:
        mid = sorted(r["delay_mid_ms"] for r in valid)
        report.update({
            "delay_mid_median_ms": statistics.median(mid),
            "delay_mid_p95_ms": mid[math.ceil(.95*len(mid))-1],
            "delay_low_median_ms": statistics.median(r["delay_low_ms"] for r in valid),
            "delay_high_median_ms": statistics.median(r["delay_high_ms"] for r in valid),
            "repeated_sample_markers": sum(a["source_ms"] == b["source_ms"] for a,b in zip(valid, valid[1:])),
            "source_frame_advances": [(b["sequence"]-a["sequence"]) % 65536 for a,b in zip(valid, valid[1:])]})
    return report


def reanalyze(directory):
    old_report = json.loads((directory / "report.json").read_text())
    source_clock = old_report.get('source_clock', 'mac')
    with (directory / "samples.csv").open() as file:
        rows = list(csv.DictReader(file))
    for row in rows:
        for key in ("source_ms", "sequence", "source_page_id", "delay_low_ms", "delay_high_ms", "delay_mid_ms"):
            row.pop(key, None)
        for key in ("index", "tv_start_us", "tv_end_us", "host_receive_us"):
            row[key] = float(row[key])
        row["index"] = int(row["index"])
        picture = Image.open(directory / f"frame-{row['index']:04d}.png").convert("RGBA")
        try:
            short_ms, sequence, page_id = decode_marker_info(picture.tobytes("raw", "BGRA"), *picture.size)
            reference = row['tv_end_us'] if source_clock == 'tv' else row['host_receive_us']
            source_ms = short_ms + round((reference / 1000-short_ms) / 2**32) * 2**32
            row.update(source_ms=source_ms, sequence=sequence, source_page_id=page_id)
            row.pop("decode_error", None)
        except ValueError as error:
            row["decode_error"] = str(error)
    report = summarize(rows, json.loads((directory / "clock-sync.json").read_text()),
                       old_report.get('source_uncertainty_ms', 0), old_report.get('source_label', 'Mac'),
                       source_clock)
    with (directory / "samples.csv").open("w") as file:
        writer = csv.DictWriter(file, fieldnames=sorted(set().union(*(r.keys() for r in rows))))
        writer.writeheader()
        writer.writerows(rows)
    old_report.update(report)
    (directory / "report.json").write_text(json.dumps(old_report, indent=2)+"\n")
    print(json.dumps(old_report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--interval", type=float, default=.25)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--type", type=int, choices=[2, 16], default=16)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--reanalyze", type=Path, help="Re-read saved captures; no device access")
    parser.add_argument("--no-marker", action="store_true")
    parser.add_argument('--inner-timing', action='store_true',
                        help='Use the separate instrumented capture build; log inner call timing')
    parser.add_argument("--source-label", default='Mac')
    parser.add_argument('--source-clock', choices=['mac', 'tv'], default='mac',
                        help='TV requires a marker calibrated by the temporary TV server')
    parser.add_argument("--source-uncertainty-ms", type=float, default=0,
                        help='Additional +/- source clock error for an external marker')
    args = parser.parse_args()
    if args.reanalyze:
        reanalyze(args.reanalyze)
        return
    if args.source_clock == 'tv' and not args.no_marker:
        parser.error('--source-clock tv requires --no-marker and a TV-calibrated source')
    if args.output is None:
        parser.error("--output is required for recording")
    if not (0 < args.seconds <= 60 and .1 <= args.interval <= 5):
        parser.error("Use 0-60 seconds and an interval from 0.1 to 5 seconds")
    if not (math.isfinite(args.source_uncertainty_ms) and 0 <= args.source_uncertainty_ms <= 100):
        parser.error('Source uncertainty must be 0-100 ms')
    if not args.no_marker and (args.source_label != 'Mac' or args.source_uncertainty_ms):
        parser.error('External source options require --no-marker')
    args.output.mkdir(parents=True, exist_ok=False)
    recorder = Capture(args.output, args.width, args.height, args.type, args.inner_timing)
    marker = None
    rows = []
    try:
        offsets = recorder.ping(20)
        if not args.no_marker:
            before = time.monotonic_ns() / 1_000_000
            swift_clock = int(subprocess.check_output([ROOT / "time-marker", "--clock"]))
            after = time.monotonic_ns() / 1_000_000
            if not before-1 <= swift_clock <= after+1:
                raise RuntimeError("Source and recorder clocks do not agree")
            marker = subprocess.Popen([ROOT / "time-marker", str(args.seconds+10)],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(1.5)
        begin = time.monotonic()
        while time.monotonic()-begin < args.seconds:
            index = len(rows)
            start, end, width, height, raw = recorder.frame(index)
            row = {"index": index, "tv_start_us": start, "tv_end_us": end,
                   "host_receive_us": time.monotonic_ns() / 1000}
            Image.frombytes("RGBA", (width, height), raw, "raw", "BGRA").convert("RGB").save(
                args.output / f"frame-{index:04d}.png")
            try:
                short_ms, sequence, page_id = decode_marker_info(raw, width, height)
                now_ms = end / 1000 if args.source_clock == 'tv' else time.monotonic_ns() / 1_000_000
                full_ms = short_ms + round((now_ms-short_ms) / 2**32) * 2**32
                row.update(source_ms=full_ms, sequence=sequence, source_page_id=page_id)
            except ValueError as error:
                row["decode_error"] = str(error)
            rows.append(row)
            target = begin + len(rows)*args.interval
            time.sleep(max(0, target-time.monotonic()))
        offsets.extend(recorder.ping(10))
        # Preserve raw timing even if the clock check fails.
        (args.output / "clock-sync.json").write_text(json.dumps(offsets, indent=2)+"\n")
        with (args.output / "samples.csv").open("w") as file:
            fields = sorted(set().union(*(r.keys() for r in rows)))
            writer = csv.DictWriter(file, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)
        report = summarize(rows, offsets, args.source_uncertainty_ms, args.source_label, args.source_clock)
        report.update(capture_type=args.type, sample_interval_seconds=args.interval,
                      inner_capture_timing=args.inner_timing,
                      sample_width=args.width, sample_height=args.height,
                      capture_plane="video+OSD" if args.type == 16 else "video only")
        (args.output / "report.json").write_text(json.dumps(report, indent=2)+"\n")
        (args.output / "clock-sync.json").write_text(json.dumps(offsets, indent=2)+"\n")
        with (args.output / "samples.csv").open("w") as file:
            fields = sorted(set().union(*(r.keys() for r in rows)))
            writer = csv.DictWriter(file, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)
        # FFmpeg concat preserves the sample cadence, not the TV's native FPS.
        lines = []
        for i, row in enumerate(rows):
            lines.append(f"file 'frame-{i:04d}.png'")
            if i+1 < len(rows):
                duration = (rows[i+1]["tv_start_us"]-row["tv_start_us"]) / 1_000_000
            else:
                duration = args.interval
            lines.append(f"duration {duration:.6f}")
        if rows:
            lines.append(f"file 'frame-{len(rows)-1:04d}.png'")
        (args.output / "recording.ffconcat").write_text("\n".join(lines)+"\n")
        print(json.dumps(report, indent=2))
    finally:
        if marker and marker.poll() is None:
            marker.terminate()
            marker.wait(timeout=5)
        recorder.close()


if __name__ == "__main__":
    main()
