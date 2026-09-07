import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen


class MarkerServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='moonlight-marker-test-')
        cls.saved = Path(cls.temp.name) / 'session.json'
        cls.process = subprocess.Popen([
            sys.executable, str(Path(__file__).with_name('web-marker.py')),
            '--bind', '127.0.0.1', '--client', '127.0.0.1', '--port', '0',
            '--seconds', '60', '--output', str(cls.saved),
        ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        info = json.loads(cls.process.stdout.readline())
        cls.base = info['url'].removesuffix('/marker')

    @classmethod
    def tearDownClass(cls):
        cls.process.terminate()
        cls.process.communicate(timeout=5)
        cls.temp.cleanup()

    def get(self, path):
        with urlopen(self.base + path, timeout=3) as reply:
            return json.load(reply)

    def post(self, path, data):
        request = Request(self.base + path, json.dumps(data).encode(),
                          {'Content-Type': 'application/json'})
        with urlopen(request, timeout=3) as reply:
            return json.load(reply)

    def test_clock_order(self):
        clock = self.get('/clock')
        self.assertLessEqual(clock['receive_ms'], clock['send_ms'])

    def test_calibration_history_and_persistence(self):
        for low in (100, 101):
            self.assertTrue(self.post('/calibration', {
                'offset_low_ms': low, 'offset_high_ms': low + 4,
                'render_offset_ms': 102,
            })['ok'])
        status = self.get('/status')
        self.assertEqual(status['calibration']['render_offset_ms'], 102)
        self.assertEqual(len(status['calibrations']), 2)
        self.assertEqual(json.loads(self.saved.read_text())['calibrations'],
                         status['calibrations'])

    def test_invalid_bounds_rejected(self):
        for low, high in ((0, 16), (4, 3), (float('nan'), 2)):
            with self.assertRaises(HTTPError) as raised:
                self.post('/calibration', {'offset_low_ms': low, 'offset_high_ms': high})
            self.assertEqual(raised.exception.code, 400)

    def test_report_route_is_text_only(self):
        self.assertTrue(self.post('/report', {'report': 'marker-ready'})['ok'])
        self.assertEqual(self.get('/status')['reports'][-1]['report'], 'marker-ready')
        with self.assertRaises(HTTPError) as raised:
            self.post('/report', {'command': 'not-supported'})
        self.assertEqual(raised.exception.code, 400)

    def test_no_other_routes(self):
        for path in ('/files', '/command', '/../clock'):
            with self.assertRaises(HTTPError) as raised:
                self.get(path)
            self.assertEqual(raised.exception.code, 404)


if __name__ == '__main__':
    unittest.main()
