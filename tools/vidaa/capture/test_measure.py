import binascii
import unittest

import numpy as np

from measure import decode_marker, decode_marker_info, summarize


def fixture(milliseconds=123456789, sequence=4321, warm=False, corrupt=False, page_id=None):
    image = np.zeros((360, 640, 4), dtype=np.uint8)
    image[:, :, 3] = 255
    color = (163, 218, 255) if warm else (255, 255, 255)
    left, right, top, bottom = 88, 552, 61, 299
    image[top:bottom, left:right, :3] = color
    grid_left, grid_top = left + (right-left)*.05, top + (bottom-top)*.05
    columns = 10 if page_id is None else 14
    cell_width, cell_height = (right-left)*.9/columns, (bottom-top)*.9/8
    payload = (b"ML" if page_id is None else b"MN") + milliseconds.to_bytes(4, "big") + sequence.to_bytes(2, "big")
    if page_id is not None:
        payload += page_id.to_bytes(4, 'big')
    payload += binascii.crc_hqx(payload, 0xffff).to_bytes(2, "big")
    for bit in range(columns * 8):
        is_white = bool(payload[bit//8] & (1 << (7-bit%8)))
        if corrupt and bit == 22:
            is_white = not is_white
        x0, x1 = round(grid_left + bit%columns*cell_width), round(grid_left + (bit%columns+1)*cell_width)
        y0, y1 = round(grid_top + bit//columns*cell_height), round(grid_top + (bit//columns+1)*cell_height)
        image[y0:y1, x0:x1, :3] = color if is_white else (0, 0, 0)
    image[320:324, 610:614, :3] = 255  # Pointer outside the marker.
    return image.tobytes()


class MarkerTests(unittest.TestCase):
    def test_page_identity_is_in_pixels_and_covered_by_crc(self):
        for page_id in (0, 17, 0xffffffff):
            self.assertEqual(decode_marker_info(fixture(page_id=page_id, warm=True), 640, 360),
                             (123456789, 4321, page_id))
        with self.assertRaisesRegex(ValueError, 'checksum'):
            decode_marker_info(fixture(page_id=17, corrupt=True), 640, 360)
    def test_normal(self):
        self.assertEqual(decode_marker(fixture(), 640, 360), (123456789, 4321))

    def test_warm_color_filter_and_pointer(self):
        self.assertEqual(decode_marker(fixture(warm=True), 640, 360), (123456789, 4321))

    def test_checksum_rejects_changed_bit(self):
        with self.assertRaisesRegex(ValueError, "checksum"):
            decode_marker(fixture(corrupt=True), 640, 360)

    def test_black_frame_is_not_a_marker(self):
        with self.assertRaises(ValueError):
            decode_marker(bytes(640*360*4), 640, 360)

    def test_timestamp_range(self):
        for value in (0, 1, 0x7fffffff, 0xffffffff):
            self.assertEqual(decode_marker(fixture(milliseconds=value), 640, 360)[0], value)

    def test_delay_bounds(self):
        rows = [{"source_ms": 10, "sequence": 1, "tv_start_us": 1_050_000, "tv_end_us": 1_080_000}]
        offsets = [{"offset_low_us": 999_000, "offset_high_us": 1_001_000}]
        result = summarize(rows, offsets)
        self.assertEqual(rows[0]["delay_low_ms"], 38)
        self.assertEqual(rows[0]["delay_high_ms"], 71)
        self.assertEqual(result["clock_uncertainty_ms"], 2)

    def test_conflicting_clock_bounds_fail(self):
        with self.assertRaisesRegex(RuntimeError, "Clock bounds conflict"):
            summarize([], [{"offset_low_us": 2, "offset_high_us": 1}])

    def test_external_source_uncertainty(self):
        rows = [{"source_ms": 10, "sequence": 1, "tv_start_us": 1_050_000, "tv_end_us": 1_080_000}]
        result = summarize(rows, [{"offset_low_us": 999_000, "offset_high_us": 1_001_000}], 3, 'Windows browser')
        self.assertEqual(rows[0]['delay_low_ms'], 35)
        self.assertEqual(rows[0]['delay_high_ms'], 74)
        self.assertEqual(rows[0]['delay_mid_ms'], 54.5)
        self.assertEqual(result['source_uncertainty_ms'], 3)
        self.assertIn('Windows browser', result['metric'])

    def test_invalid_external_source_uncertainty(self):
        with self.assertRaises(ValueError):
            summarize([], [], float('nan'))

    def test_tv_clock_ignores_unrelated_mac_clock_step(self):
        rows = [dict(source_ms=1000, sequence=1, tv_start_us=1050000, tv_end_us=1080000)]
        result = summarize(rows, [dict(offset_low_us=99, offset_high_us=-99)],
                           3, 'Windows browser', 'tv')
        self.assertEqual(result['source_clock'], 'tv')
        self.assertEqual(result['clock_offset_low_us'], 0)
        self.assertEqual(rows[0]['delay_low_ms'], 46)
        self.assertEqual(rows[0]['delay_high_ms'], 83)


if __name__ == "__main__":
    unittest.main()
