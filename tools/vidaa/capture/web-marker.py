#!/usr/bin/env python3
"""Temporary fixed-route marker server. No file browsing or command execution."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
from pathlib import Path
import secrets
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind', default='192.168.1.132')
    parser.add_argument('--client', default='192.168.1.139')
    parser.add_argument('--port', type=int, default=47986)
    parser.add_argument('--seconds', type=int, default=900, choices=range(60, 3601),
                        metavar='60..3600')
    parser.add_argument('--output', type=Path, help='Save this session diagnostic JSON locally')
    args = parser.parse_args()
    page = Path(__file__).with_name('web-marker.html').read_bytes()
    token = secrets.token_urlsafe(24)
    state = {'calibration': None, 'calibrations': [], 'reports': []}
    state_lock = threading.Lock()

    def save_state():
        if args.output:
            args.output.write_text(json.dumps(state, indent=2))

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def send(self, code, value, content_type='application/json'):
            body = value if isinstance(value, bytes) else json.dumps(value).encode()
            self.send_response(code)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.end_headers()
            self.wfile.write(body)

        def route(self):
            if self.client_address[0] not in (args.bind, args.client):
                return None
            prefix = '/' + token
            return self.path[len(prefix):] if self.path.startswith(prefix+'/') else None

        def do_GET(self):
            received = time.monotonic_ns()/1e6
            route = self.route()
            if route == '/marker':
                self.send(200, page, 'text/html; charset=utf-8')
            elif route == '/clock':
                self.send(200, {'receive_ms': received, 'send_ms': time.monotonic_ns()/1e6})
            elif route == '/status':
                with state_lock:
                    self.send(200, dict(state, now_ms=time.monotonic_ns()/1e6))
            else:
                self.send(404, {})

        def do_POST(self):
            if self.route() == '/report':
                try:
                    size = int(self.headers.get('Content-Length', '0'))
                    if not 0 < size <= 16384:
                        raise ValueError()
                    data = json.loads(self.rfile.read(size))
                    if not isinstance(data, dict) or not isinstance(data.get('report'), str):
                        raise ValueError()
                    with state_lock:
                        state['reports'].append({'report': data['report'],
                            'source_ip':self.client_address[0], 'server_time_ms':time.monotonic_ns()/1e6})
                        state['reports'] = state['reports'][-10:]
                        save_state()
                    self.send(200, {'ok':True})
                except (ValueError, TypeError):
                    self.send(400, {'ok':False})
                return
            if self.route() != '/calibration':
                self.send(404, {})
                return
            try:
                size = int(self.headers.get('Content-Length','0'))
                if not 0 < size <= 16384:
                    raise ValueError()
                data = json.loads(self.rfile.read(size))
                low, high = float(data['offset_low_ms']), float(data['offset_high_ms'])
                if not (math.isfinite(low) and math.isfinite(high) and 0 <= high-low <= 15):
                    raise ValueError()
                if 'render_offset_ms' in data and not math.isfinite(float(data['render_offset_ms'])):
                    raise ValueError()
                with state_lock:
                    state['calibration'] = dict(data, server_time_ms=time.monotonic_ns()/1e6,
                                                source_ip=self.client_address[0])
                    state['calibrations'].append(state['calibration'])
                    state['calibrations'] = state['calibrations'][-180:]
                    save_state()
                self.send(200, {'ok': True})
            except (ValueError, KeyError, TypeError):
                self.send(400, {'ok': False})

    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    server.daemon_threads = True
    timer = threading.Timer(args.seconds, server.shutdown)
    timer.daemon = True
    timer.start()
    port = server.server_address[1]
    print(json.dumps({'url':f'http://{args.bind}:{port}/{token}/marker',
                      'status_url':f'http://{args.bind}:{port}/{token}/status'}), flush=True)
    try:
        server.serve_forever()
    finally:
        server.server_close()
        timer.cancel()
        with state_lock:
            save_state()


if __name__ == '__main__':
    main()
