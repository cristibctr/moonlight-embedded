"""Validate browser clock checks before applying an external latency result."""
import argparse
import csv
import json
import math
from pathlib import Path


def read_checks(path):
    if path.suffix != '.jsonl':
        return json.loads(path.read_text()).get('calibrations', [])
    checks = []
    for line in path.read_text().splitlines():
        item = json.loads(line)
        if item.get('route') == 'calibration':
            checks.append(dict(item['data'], server_time_ms=item['server_time_ms'],
                               source_ip=item['source_ip']))
    return checks


def validate_clock(checks, start_ms, end_ms, source_ip='192.168.1.139',
                   drift_ppm=100, max_gap_ms=45000, viewport=None, page_id=None):
    if not all(math.isfinite(v) for v in (start_ms, end_ms, drift_ppm, max_gap_ms)):
        raise ValueError('Clock inputs must be finite')
    if start_ms > end_ms or not 0 <= drift_ppm <= 1000 or max_gap_ms <= 0:
        raise ValueError('Invalid clock interval or drift allowance')
    points = sorted((p for p in checks if p.get('source_ip') == source_ip and
                     (page_id is None or p.get('page_id') == page_id) and
                     (viewport is None or p.get('viewport') == list(viewport))),
                    key=lambda p: p['server_time_ms'])
    before = [i for i, p in enumerate(points) if p['server_time_ms'] <= start_ms]
    after = [i for i, p in enumerate(points) if p['server_time_ms'] >= end_ms]
    if not before or not after:
        raise ValueError('Need source clock checks before AND after the capture')
    points = points[before[-1]:after[0] + 1]
    if len(points) < 2:
        raise ValueError('Need two distinct clock checks')
    rendered = float(points[0]['render_offset_ms'])
    deviation = 0.0
    for p in points:
        low, high = float(p['offset_low_ms']), float(p['offset_high_ms'])
        values = (p['server_time_ms'], low, high, p['render_offset_ms'])
        if not all(math.isfinite(v) for v in values) or not 0 <= high-low <= 15:
            raise ValueError('Invalid source clock check')
        if abs(p['render_offset_ms']-rendered) > 1e-6:
            raise ValueError('Source page reloaded or rendered clock changed during the run')
        deviation = max(deviation, abs(rendered-low), abs(high-rendered))
    gap = max(b['server_time_ms']-a['server_time_ms'] for a, b in zip(points, points[1:]))
    if gap > max_gap_ms:
        raise ValueError('Source clock checks are too far apart')
    # A conservative full-gap allowance, explicitly assumed, not measured.
    allowance = gap * drift_ppm / 1e6
    # A stale rendered clock can be minutes ahead after a host clock step.
    # Do not publish a negative midpoint with a huge uncertainty interval.
    if deviation + allowance > 15:
        raise ValueError('Rendered source clock drift exceeds 15 ms; reload the marker and repeat')
    return {
        'source_ip': source_ip, 'checks': len(points), 'render_offset_ms': rendered,
        'source_viewport_filter': viewport,
        'source_page_id': page_id,
        'capture_start_ms': start_ms, 'capture_end_ms': end_ms,
        'first_check_ms': points[0]['server_time_ms'],
        'last_check_ms': points[-1]['server_time_ms'],
        'largest_check_gap_ms': gap, 'observed_offset_error_bound_ms': deviation,
        'assumed_drift_ppm': drift_ppm, 'between_check_allowance_ms': allowance,
        'source_uncertainty_ms': deviation + allowance,
        'limits': 'Checks bracket the run. Drift between checks is assumed, not measured. '
                  'This validates clock accounting, not optical delay or frame pacing.',
    }


def input_timing(row, source_ms, report):
    extra = report.get('source_uncertainty_ms', 0)
    low_offset, high_offset = report['clock_offset_low_us'], report['clock_offset_high_us']
    if (not all(math.isfinite(v) for v in (extra, low_offset, high_offset, source_ms))
            or extra < 0 or low_offset > high_offset):
        raise ValueError('Invalid input clock bounds')
    low = (row['receive_us']-high_offset)/1000 - (source_ms+1) - extra
    high = (row['receive_us']-low_offset)/1000 - source_ms + extra
    return {
        'host_to_receive_low_ms': low, 'host_to_receive_high_ms': high,
        'host_to_receive_ms': (low+high)/2,
        'receive_to_submit_ms': (row['submit_us']-row['receive_us'])/1000,
        'submit_call_ms': (row['complete_us']-row['submit_us'])/1000,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('session', type=Path)
    parser.add_argument('--drift-ppm', type=float, default=100,
                        help='Assumed maximum drift between checks; not a measured bound')
    parser.add_argument('--source-ip', default='192.168.1.139')
    parser.add_argument('--viewport', type=int, nargs=2, metavar=('WIDTH', 'HEIGHT'),
                        help='Use only the independently observed visible browser viewport')
    parser.add_argument('--apply', action='store_true', help='Update and reanalyze the saved run')
    args = parser.parse_args()
    report = json.loads((args.run / 'report.json').read_text())
    with (args.run / 'samples.csv').open() as file:
        rows = [r for r in csv.DictReader(file) if r.get('source_ms')]
    if not rows:
        parser.error('No valid markers in the capture')
    page_ids = {int(r['source_page_id']) for r in rows if r.get('source_page_id')}
    if len(page_ids) > 1 or (page_ids and any(not r.get('source_page_id') for r in rows)):
        parser.error('Captured more than one source page; repeat with a stable visible page')
    page_id = next(iter(page_ids), None)
    start_ms = (min(float(r['tv_start_us']) for r in rows)-report['clock_offset_high_us'])/1000
    end_ms = (max(float(r['tv_end_us']) for r in rows)-report['clock_offset_low_us'])/1000
    try:
        result = validate_clock(read_checks(args.session), start_ms, end_ms,
                                args.source_ip, args.drift_ppm, viewport=args.viewport, page_id=page_id)
    except (ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    if args.apply:
        from measure import reanalyze
        report.update(source_label='Windows browser',
                      source_uncertainty_ms=result['source_uncertainty_ms'],
                      source_clock_note=result['limits'])
        (args.run / 'source-clock-validation.json').write_text(json.dumps(result, indent=2)+'\n')
        (args.run / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
        reanalyze(args.run)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
