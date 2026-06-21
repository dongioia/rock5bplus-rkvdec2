# scripts/vvtest/test_marker_parse.py
import unittest, marker_parse
VULKAN = 'creating element "vulkanh264dec"\nSetting pipeline to PLAYING\nGot EOS'
V4L2_FAIL = ('creating element "v4l2slh264dec"\n'
             'DMABuf caps negotiated without the mandatory support of VideoMeta\n'
             'Failed to negotiate with downstream\nnot-negotiated (-4)')
class T(unittest.TestCase):
    def test_vulkan_ok(self):
        r = marker_parse.classify(VULKAN)
        self.assertEqual(r["decoder"], "vulkanh264dec"); self.assertTrue(r["hw"]); self.assertTrue(r["negotiated"]); self.assertFalse(r["videometa_fail"])
    def test_v4l2_videometa_fail(self):
        r = marker_parse.classify(V4L2_FAIL)
        self.assertEqual(r["decoder"], "v4l2slh264dec"); self.assertTrue(r["hw"]); self.assertFalse(r["negotiated"]); self.assertTrue(r["videometa_fail"])
    def test_none(self):
        r = marker_parse.classify("no decoder here")
        self.assertEqual(r["decoder"], "none"); self.assertFalse(r["hw"])
if __name__ == "__main__": unittest.main()
