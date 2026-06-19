import math
import unittest

import nv12_tool as t


class TestNormalize(unittest.TestCase):
    def test_strips_y_and_uv_padding(self):
        # stride_y=8, visible 4x2, UV plane starts at stride_y*buf_h=16.
        # Y rows: bytes 0..3 then pad to 8; UV rows: bytes 0..3 then pad.
        raw = bytearray(64)
        for y in range(2):
            for x in range(8):
                raw[y * 8 + x] = (y * 10 + x) if x < 4 else 0xAA  # pad = 0xAA
        uv_off = 16
        for y in range(1):  # out_h//2 = 1 UV row
            for x in range(8):
                raw[uv_off + y * 8 + x] = (100 + x) if x < 4 else 0xBB
        out = t.normalize_nv12(bytes(raw), stride_y=8, stride_uv=8,
                               buf_h=2, out_w=4, out_h=2)
        # Y: 2 rows * 4 bytes, then UV: 1 row * 4 bytes  -> 12 bytes, no 0xAA/0xBB
        self.assertEqual(out, bytes([0, 1, 2, 3, 10, 11, 12, 13, 100, 101, 102, 103]))

    def test_uv_row_is_w_bytes_not_half(self):
        # NV12 chroma row = out_w bytes (out_w/2 interleaved CbCr pairs), NOT out_w/2.
        raw = bytes(4096)
        out = t.normalize_nv12(raw, stride_y=64, stride_uv=64,
                               buf_h=16, out_w=16, out_h=8)
        self.assertEqual(len(out), 16 * 8 + 16 * (8 // 2))  # Y + UV(W bytes * H/2)

    def test_raises_when_raw_too_small(self):
        with self.assertRaises(ValueError):
            t.normalize_nv12(b"\x00" * 4, stride_y=8, stride_uv=8,
                             buf_h=2, out_w=4, out_h=2)


class TestCompare(unittest.TestCase):
    def test_byte_exact(self):
        a = bytes(range(256))
        r = t.compare(a, bytes(range(256)))
        self.assertTrue(r["byte_exact"])

    def test_psnr_inf_when_equal(self):
        self.assertEqual(t.psnr(bytes([5, 5, 5]), bytes([5, 5, 5])), math.inf)

    def test_psnr_finite_and_first_diff(self):
        r = t.compare(bytes([10, 10, 10, 10]), bytes([10, 12, 10, 10]))
        self.assertFalse(r["byte_exact"])
        self.assertEqual(r["first_diff"], 1)
        self.assertTrue(0 < r["psnr_db"] < math.inf)

    def test_blank_detection(self):
        self.assertEqual(t.distinct_values(bytes([7] * 1000)), 1)
        self.assertGreater(t.distinct_values(bytes(range(256)) * 4), 200)


if __name__ == "__main__":
    unittest.main()
