import os, struct, unittest, tempfile
import pixelcheck

def _w(b): f=tempfile.NamedTemporaryFile(delete=False, suffix=".raw"); f.write(b); f.close(); return f.name

class T(unittest.TestCase):
    def test_byte_exact_pass(self):
        data = bytes(range(256)) * 4
        a, b = _w(data), _w(data)
        r = pixelcheck.compare_region(a, b, 0, 0, 32, 32)  # geometry meta ignored for raw equal-length
        self.assertEqual(r["verdict"], "PASS"); self.assertEqual(r["metric"], "byte-exact")
    def test_blank_fails_even_if_equal(self):
        data = bytes([16]) * 1024  # one luma value = blank
        a, b = _w(data), _w(data)
        r = pixelcheck.compare_region(a, b, 0, 0, 32, 32)
        self.assertEqual(r["verdict"], "FAIL"); self.assertIn("blank", r["reason"])
    def test_near_miss_psnr_pass(self):
        base = bytes(range(256)) * 4
        noisy = bytes((x + (1 if i % 50 == 0 else 0)) & 0xff for i, x in enumerate(base))
        a, b = _w(base), _w(noisy)
        r = pixelcheck.compare_region(a, b, 0, 0, 32, 32)
        self.assertEqual(r["verdict"], "PASS"); self.assertEqual(r["metric"], "psnr"); self.assertGreaterEqual(r["psnr"], 50)
    def test_garbage_fails(self):
        a = _w(bytes(range(256)) * 4)
        b = _w(bytes((255 - x) for x in (bytes(range(256)) * 4)))
        r = pixelcheck.compare_region(a, b, 0, 0, 32, 32)
        self.assertEqual(r["verdict"], "FAIL")

if __name__ == "__main__": unittest.main()
