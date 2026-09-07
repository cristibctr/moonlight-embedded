import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from source_clock import read_checks


class TvMarkerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='moonlight-tv-marker-test-')
        root = Path(__file__).resolve().parent
        cls.binary = Path(cls.temp.name) / 'server'
        subprocess.run(['cc', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(root / 'tv-marker-server.c'),
                        '-o', str(cls.binary)], check=True)
        cls.saved = Path(cls.temp.name) / 'session.jsonl'
        cls.process = subprocess.Popen([str(cls.binary), '127.0.0.1', '127.0.0.1',
            '127.0.0.1', '0', '60', 'test-token-' + 'a'*32, str(root / 'web-marker.html'),
            str(cls.saved)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        info = json.loads(cls.process.stdout.readline())
        cls.base = info['url'].removesuffix('/marker')

    @classmethod
    def tearDownClass(cls):
        cls.process.terminate()
        _, errors = cls.process.communicate(timeout=5)
        if errors:
            raise AssertionError(errors)
        cls.temp.cleanup()

    def test_clock_and_page(self):
        with urlopen(self.base + '/clock', timeout=3) as reply:
            result = json.load(reply)
        self.assertEqual(result['source_clock'], 'tv')
        self.assertLessEqual(result['receive_ms'], result['send_ms'])
        with urlopen(self.base + '/marker', timeout=3) as reply:
            self.assertIn(b'requestAnimationFrame', reply.read())

    def test_fixed_routes_and_token(self):
        for url in (self.base + '/files', self.base + '/command',
                    self.base.replace('test-token-', 'wrong-token-') + '/clock'):
            with self.assertRaises(HTTPError) as result:
                urlopen(url, timeout=3)
            self.assertEqual(result.exception.code, 404)

    def test_bounded_json_log_and_source_identity(self):
        data = dict(offset_low_ms=10, offset_high_ms=12, render_offset_ms=11,
                    source_ip='not-trusted', server_time_ms=-1)
        request = Request(self.base + '/calibration', json.dumps(data).encode(),
                          {'Content-Type': 'application/json'})
        with urlopen(request, timeout=3) as reply:
            self.assertTrue(json.load(reply)['ok'])
        checks = read_checks(self.saved)
        self.assertEqual(checks[-1]['source_ip'], '127.0.0.1')
        self.assertGreater(checks[-1]['server_time_ms'], 0)
        huge = Request(self.base + '/calibration', b'{' + b' '*17000 + b'}')
        with self.assertRaises(HTTPError) as result:
            urlopen(huge, timeout=3)
        self.assertEqual(result.exception.code, 400)

    def test_expiry_and_client_filter(self):
        root = Path(__file__).resolve().parent
        process = subprocess.Popen([str(self.binary), '127.0.0.1', '127.0.0.2',
            '127.0.0.3', '0', '1', 'test-token-' + 'b'*32, str(root / 'web-marker.html'),
            str(Path(self.temp.name) / 'expiry.jsonl')], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True)
        info = json.loads(process.stdout.readline())
        try:
            with self.assertRaises((ConnectionError, OSError)):
                urlopen(info['url'], timeout=2)
            _, errors = process.communicate(timeout=4)
            self.assertEqual(process.returncode, 0)
            self.assertFalse(errors)
        finally:
            if process.poll() is None:
                process.terminate()
                process.communicate(timeout=4)


if __name__ == '__main__':
    unittest.main()
