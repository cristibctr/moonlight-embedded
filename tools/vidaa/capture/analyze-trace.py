#!/usr/bin/env python3
"""Match compressed TV input frames to captured TV output markers."""
import argparse
import csv
import json
import os
from pathlib import Path
import statistics
import subprocess

from measure import decode_marker_info
from source_clock import input_timing


def trace_timing(row, source_ms, report, tv_only=False):
    if not tv_only:
        return input_timing(row, source_ms, report)
    # These three timestamps share the TV monotonic clock. The source marker
    # remains an identity only; no browser clock accuracy is needed here.
    return {
        'receive_to_submit_ms': (row['submit_us']-row['receive_us'])/1000,
        'submit_call_ms': (row['complete_us']-row['submit_us'])/1000,
    }


def require_source_validation(directory, report):
    if report.get('source_clock') != 'tv':
        return
    path = directory / 'source-clock-validation.json'
    if not path.exists():
        raise ValueError('Validate the source clock first, or use --tv-only')
    validation = json.loads(path.read_text())
    if validation['source_uncertainty_ms'] != report['source_uncertainty_ms']:
        raise ValueError('Report does not match the saved source clock validation')
    with (directory / 'samples.csv').open() as file:
        rows = [r for r in csv.DictReader(file) if r.get('source_ms')]
    if not rows:
        raise ValueError('No valid captured markers')
    start = min(float(r['tv_start_us']) for r in rows)/1000
    end = max(float(r['tv_end_us']) for r in rows)/1000
    if (start != validation['capture_start_ms'] or end != validation['capture_end_ms']):
        raise ValueError('Source clock validation belongs to a different capture')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--codec", choices=["hevc", "h264"], default="hevc")
    parser.add_argument("--tv-only", action="store_true",
                        help="Match TV input/output only; suppress unvalidated host delay")
    args = parser.parse_args()
    directory = args.directory
    ffmpeg = os.environ.get("MOONLIGHT_FFMPEG", "ffmpeg")
    with (directory / "incoming.csv").open() as file:
        input_rows = [{k:int(v) for k,v in r.items()} for r in csv.DictReader(file)]
    report = json.loads((directory / "report.json").read_text())
    if not args.tv_only:
        require_source_validation(directory, report)
    suffix = '-tv-only' if args.tv_only else ''
    offset = (report["clock_offset_low_us"]+report["clock_offset_high_us"]) / 2
    log_path = directory / "offline-decode.log"
    decoded = []
    with log_path.open("wb") as log:
        process = subprocess.Popen([str(ffmpeg), "-hide_banner", "-f", args.codec,
            "-i", str(directory / "incoming.hevc"), "-vf", "showinfo,scale=640:360",
            "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt", "bgra", "-"],
            stdout=subprocess.PIPE, stderr=log)
        index = 0
        while True:
            frame = process.stdout.read(640*360*4)
            if not frame:
                break
            if len(frame) != 640*360*4 or index >= len(input_rows):
                process.terminate()
                raise RuntimeError("Input/decode frame count mismatch")
            row = dict(input_rows[index])
            index += 1
            try:
                short_ms, sequence, page_id = decode_marker_info(frame, 640, 360)
                host_receive_ms = (row["receive_us"]-offset) / 1000
                source_ms = short_ms + round((host_receive_ms-short_ms) / 2**32)*2**32
                row.update(source_ms=source_ms, sequence=sequence, source_page_id=page_id,
                           **trace_timing(row, source_ms, report, args.tv_only))
                decoded.append(row)
            except ValueError:
                pass  # Stream startup/desktop before the marker opens.
        if process.wait() != 0 or index != len(input_rows):
            raise RuntimeError(f"Decoder returned {index} frames for {len(input_rows)} input rows")
    if "type:B" in log_path.read_text():
        raise RuntimeError("B-frame reorder requires POC mapping; do not use decode order")
    if not decoded:
        raise RuntimeError("No markers decoded from incoming stream")
    with (directory / f"decoded-input{suffix}.csv").open("w") as file:
        writer = csv.DictWriter(file, fieldnames=decoded[0].keys())
        writer.writeheader()
        writer.writerows(decoded)
    by_marker = {}
    for row in decoded:
        by_marker.setdefault((row["source_ms"], row["sequence"], row['source_page_id']), []).append(row)
    matched = []
    with (directory / "samples.csv").open() as file:
        for sample in csv.DictReader(file):
            if not sample.get("source_ms"):
                continue
            page_id = int(sample['source_page_id']) if sample.get('source_page_id') else None
            candidates = by_marker.get((int(float(sample["source_ms"])), int(sample["sequence"]), page_id), [])
            candidates = [r for r in candidates if r["receive_us"] <= float(sample["tv_end_us"])]
            if not candidates:
                continue
            earliest = min(r["receive_us"] for r in candidates)
            latest = max(r["receive_us"] for r in candidates)
            low = (float(sample["tv_start_us"])-latest) / 1000
            high = (float(sample["tv_end_us"])-earliest) / 1000
            matched.append({"capture_index": sample["index"], "source_ms": sample["source_ms"],
                "sequence": sample["sequence"], "source_page_id": page_id,
                "input_matches": len(candidates),
                "tv_residence_low_ms": low, "tv_residence_high_ms": high,
                "tv_residence_mid_ms": (low+high)/2})
    if matched:
        with (directory / f"matched-output{suffix}.csv").open("w") as file:
            writer = csv.DictWriter(file, fieldnames=matched[0].keys())
            writer.writeheader()
            writer.writerows(matched)
    result = {"input_frames": len(input_rows), "decoded_markers": len(decoded),
              "matched_output_samples": len(matched), "b_frames": False,
              "scope": "tv-input-to-capture-only" if args.tv_only else "source-and-tv",
              "receive_to_submit_median_ms": statistics.median(r["receive_to_submit_ms"] for r in decoded),
              "submit_call_median_ms": statistics.median(r["submit_call_ms"] for r in decoded),
              "source_label": report.get('source_label', 'Mac'),
              "limits": "Input timing includes marker draw/capture/encode/network. TV residence includes decode/display/capture uncertainty. Not optical or controller latency. Repeated encoded markers widen TV residence bounds."}
    if args.tv_only:
        result['limits'] = ('Source timestamps are used only to match identical input/output pixels. '
                            'No host or total latency is reported. TV residence includes '
                            'decode/display/capture uncertainty. Not optical or controller latency. '
                            'Repeated encoded markers widen TV residence bounds.')
    else:
        result.update(
            host_to_receive_median_ms=statistics.median(r['host_to_receive_ms'] for r in decoded),
            host_to_receive_low_median_ms=statistics.median(r['host_to_receive_low_ms'] for r in decoded),
            host_to_receive_high_median_ms=statistics.median(r['host_to_receive_high_ms'] for r in decoded),
            host_to_receive_p95_ms=sorted(r['host_to_receive_ms'] for r in decoded)[int(.95*(len(decoded)-1))],
            clock_uncertainty_ms=report['clock_uncertainty_ms'],
            source_uncertainty_ms=report.get('source_uncertainty_ms', 0))
    if matched:
        for key in ("tv_residence_low_ms", "tv_residence_high_ms", "tv_residence_mid_ms"):
            result[key.replace("_ms", "_median_ms")] = statistics.median(r[key] for r in matched)
    (directory / f"pipeline{suffix}-report.json").write_text(json.dumps(result, indent=2)+"\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
