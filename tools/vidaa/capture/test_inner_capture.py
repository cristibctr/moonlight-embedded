import unittest
from inner_capture import match_intervals


class InnerTimingTests(unittest.TestCase):
    def setUp(self):
        self.rows = [dict(index=0, tv_start_us=100, tv_end_us=200)]

    def test_contained_call(self):
        self.assertEqual(match_intervals(self.rows, 'inner_capture=0 start_us=110 end_us=190\n'),
                         {0: (110, 190)})

    def test_failures_rejected(self):
        for log in ('', 'inner_capture=0 start_us=90 end_us=190\n',
                    'inner_capture=0 start_us=110 end_us=210\n',
                    'inner_capture=0 start_us=180 end_us=170\n',
                    'inner_capture=5 start_us=110 end_us=190\n',
                    'inner_capture=0 start_us=110 end_us=190\n'*2):
            with self.subTest(log=log), self.assertRaises(ValueError):
                match_intervals(self.rows, log)


if __name__ == '__main__':
    unittest.main()
