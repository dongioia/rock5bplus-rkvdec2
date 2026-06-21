# scripts/vvtest/marker_parse.py
import re
HW = {"vulkanh264dec", "v4l2slh264dec"}
def classify(text):
    decs = re.findall(r'creating element "([a-z0-9_]*(?:h264dec|h264))"', text)
    decs = [d for d in decs if "dec" in d]
    decoder = "none"
    for pref in ("vulkanh264dec", "v4l2slh264dec", "avdec_h264", "openh264dec"):
        if pref in decs: decoder = pref; break
    return {
        "decoder": decoder,
        "hw": decoder in HW,
        "negotiated": "not-negotiated" not in text and "Failed to negotiate" not in text,
        "videometa_fail": "VideoMeta" in text,
    }
