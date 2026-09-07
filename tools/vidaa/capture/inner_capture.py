"""Bound the existing capture at MI_CAP_CaptureOne, without changing old results."""
import argparse
import csv
import json
from pathlib import Path
import re
import statistics

PATTERN = re.compile(r'^inner_capture=(-?\d+) start_us=(\d+) end_us=(\d+)$', re.M)


def match_intervals(samples, log):
    calls = [tuple(map(int, m)) for m in PATTERN.findall(log)]
    if len(calls) != len(samples):
        raise ValueError('Need exactly one inner capture call per saved sample')
    intervals = {}
    for row, (result, start, end) in zip(samples, calls):
        if result or not float(row['tv_start_us']) <= start < end <= float(row['tv_end_us']):
            raise ValueError('Inner capture failed or is outside its outer call')
        index = int(row['index'])
        if index in intervals:
            raise ValueError('Duplicate capture index')
        intervals[index] = (start, end)
    return intervals


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--tv-only', action='store_true')
    args = parser.parse_args()
    root = args.directory
    report = json.loads((root / 'report.json').read_text())
    if not report.get('inner_capture_timing'):
        parser.error('Not an instrumented capture run')
    with (root / 'samples.csv').open() as file:
        samples = list(csv.DictReader(file))
    intervals = match_intervals(samples, (root / 'tv-capture.log').read_text())
    samples = {int(r['index']): r for r in samples}
    suffix = '-tv-only' if args.tv_only else ''
    with (root / f'matched-output{suffix}.csv').open() as file:
        matched = list(csv.DictReader(file))
    results = []
    for row in matched:
        index = int(row['capture_index'])
        start, end = intervals[index]
        outer = samples[index]
        low = float(row['tv_residence_low_ms']) + (start-float(outer['tv_start_us']))/1000
        high = float(row['tv_residence_high_ms']) - (float(outer['tv_end_us'])-end)/1000
        results.append(dict(capture_index=index, tv_residence_low_ms=low,
                            tv_residence_high_ms=high, tv_residence_mid_ms=(low+high)/2))
    if not results:
        parser.error('No matched input/output markers')
    result = dict(scope='tv-input-to-inner-capture-call-only', samples=len(samples),
                  matched_samples=len(results),
                  inner_call_median_ms=statistics.median((b-a)/1000 for a,b in intervals.values()),
                  outer_call_median_ms=report['capture_duration_median_ms'],
                  limits='Call interval, not exposure time, panel latency or controller latency. '
                         'This refines measurement bounds; it is not a stream speed improvement. '
                         'Uses the same pixels and input matches as the outer capture report.')
    for key in ('tv_residence_low_ms', 'tv_residence_mid_ms', 'tv_residence_high_ms'):
        result[key.replace('_ms', '_median_ms')] = statistics.median(r[key] for r in results)
    with (root / 'inner-matched-output.csv').open('w') as file:
        writer = csv.DictWriter(file, fieldnames=results[0])
        writer.writeheader()
        writer.writerows(results)
    (root / 'inner-capture-report.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
