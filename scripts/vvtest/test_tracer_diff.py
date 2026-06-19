import unittest

import tracer_diff as td

OURS = {"trace": [
    {"ioctl": "VIDIOC_S_EXT_CTRLS", "controls": [
        {"id": "SPS", "level_idc": 31, "num_ref_frames": 1},
        {"id": "DECODE_PARAMS", "flags": 0},
    ]},
    {"ioctl": "VIDIOC_S_EXT_CTRLS", "controls": [{"id": "SPS", "level_idc": 31}]},
]}
GOLDEN = {"trace": [
    {"ioctl": "VIDIOC_S_EXT_CTRLS", "controls": [
        {"id": "SPS", "level_idc": 30, "num_ref_frames": 1},
        {"id": "DECODE_PARAMS", "flags": 0},
        {"id": "SCALING_MATRIX", "present": True},
    ]},
]}


class TestTracerDiff(unittest.TestCase):
    def test_finds_first_s_ext_ctrls(self):
        node = td.first_ioctl(OURS, "VIDIOC_S_EXT_CTRLS")
        self.assertIsNotNone(node)
        self.assertEqual(node["controls"][0]["level_idc"], 31)

    def test_value_diff_detected(self):
        diffs = td.diff_first_controls(OURS, GOLDEN)
        paths = {d[0] for d in diffs}
        kinds = {d[1] for d in diffs}
        self.assertTrue(any("level_idc" in p for p in paths))  # 31 vs 30
        self.assertIn("value", kinds)

    def test_missing_control_detected(self):
        diffs = td.diff_first_controls(OURS, GOLDEN)
        # golden has SCALING_MATRIX (controls[2]); ours' first group has only 2
        self.assertTrue(any(d[1] in ("len", "only_golden") for d in diffs))

    def test_identical_traces_no_diff(self):
        self.assertEqual(td.diff_first_controls(OURS, OURS), [])


if __name__ == "__main__":
    unittest.main()
