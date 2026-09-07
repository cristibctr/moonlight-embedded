import unittest

from source_clock import input_timing, validate_clock


def point(time_ms, low=998, high=1002, rendered=1000, ip='192.168.1.139'):
    return dict(server_time_ms=time_ms, offset_low_ms=low, offset_high_ms=high,
                render_offset_ms=rendered, source_ip=ip)


class SourceClockTests(unittest.TestCase):
    def test_brackets_with_drift_allowance(self):
        result = validate_clock([point(0), point(20000, 999, 1004)], 5000, 15000)
        self.assertEqual(result['observed_offset_error_bound_ms'], 4)
        self.assertEqual(result['between_check_allowance_ms'], 2)
        self.assertEqual(result['source_uncertainty_ms'], 6)

    def test_all_intermediate_checks_contribute(self):
        result = validate_clock([point(0), point(20000, 1003, 1008), point(40000)],
                                5000, 35000)
        self.assertEqual(result['source_uncertainty_ms'], 10)

    def test_missing_end_check_fails(self):
        with self.assertRaisesRegex(ValueError, 'before AND after'):
            validate_clock([point(0)], 5000, 15000)

    def test_wrong_host_cannot_supply_end_check(self):
        with self.assertRaisesRegex(ValueError, 'before AND after'):
            validate_clock([point(0), point(20000, ip='192.168.1.132')], 5000, 15000)

    def test_reload_fails(self):
        with self.assertRaisesRegex(ValueError, 'reloaded'):
            validate_clock([point(0), point(20000, rendered=1001)], 5000, 15000)

    def test_explicit_viewport_separates_an_unrelated_background_page(self):
        checks = [dict(point(0), viewport=[2560, 1440]),
                  dict(point(10000, rendered=2000), viewport=[1306, 850]),
                  dict(point(20000), viewport=[2560, 1440])]
        with self.assertRaisesRegex(ValueError, 'reloaded'):
            validate_clock(checks, 5000, 15000)
        result = validate_clock(checks, 5000, 15000, viewport=[2560, 1440])
        self.assertEqual(result['checks'], 2)
        self.assertEqual(result['source_viewport_filter'], [2560, 1440])

    def test_pixel_page_id_separates_equal_size_windows(self):
        checks = [dict(point(0), page_id=17), dict(point(10000, rendered=2000), page_id=18),
                  dict(point(20000), page_id=17)]
        result = validate_clock(checks, 5000, 15000, page_id=17)
        self.assertEqual(result['checks'], 2)
        self.assertEqual(result['source_page_id'], 17)

    def test_long_gap_fails(self):
        with self.assertRaisesRegex(ValueError, 'too far apart'):
            validate_clock([point(0), point(60000)], 5000, 15000)

    def test_invalid_bounds_fail(self):
        with self.assertRaisesRegex(ValueError, 'Invalid source clock'):
            validate_clock([point(0), point(20000, low=float('nan'))], 5000, 15000)

    def test_large_clock_step_fails(self):
        with self.assertRaisesRegex(ValueError, 'clock drift exceeds'):
            validate_clock([point(0), point(20000, -292000, -291998)], 5000, 15000)

    def test_stale_render_offset_fails_even_with_stable_new_checks(self):
        with self.assertRaisesRegex(ValueError, 'clock drift exceeds'):
            validate_clock([point(0, 970, 974), point(20000, 970, 974)], 5000, 15000)

    def test_input_timing_includes_both_clock_bounds_and_rounding(self):
        row = dict(receive_us=1060000, submit_us=1060100, complete_us=1060700)
        report = dict(clock_offset_low_us=1000000, clock_offset_high_us=1002000,
                      source_uncertainty_ms=3)
        timing = input_timing(row, 10, report)
        self.assertEqual(timing['host_to_receive_low_ms'], 44)
        self.assertEqual(timing['host_to_receive_high_ms'], 53)
        self.assertEqual(timing['host_to_receive_ms'], 48.5)
        self.assertEqual(timing['receive_to_submit_ms'], .1)
        self.assertEqual(timing['submit_call_ms'], .6)


if __name__ == '__main__':
    unittest.main()
