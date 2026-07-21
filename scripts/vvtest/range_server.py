#!/usr/bin/env python3
"""
range_server.py — minimal HTTP/1.1 server that honours Range: bytes=a-b
and returns 206 Partial Content, so WebKit/GStreamer can stream MP4/H264
without MEDIA_ERR_NETWORK (code=9).

Usage:
    python3 range_server.py [PORT [DIRECTORY]]
    Defaults: port 8889, directory = CWD

Binds to 127.0.0.1 only (localhost, no LAN exposure).
"""
import os
import sys
import http.server
import socketserver


class RangeHTTPRequestHandler(http.server.SimpleHTTPRequestHandler):
    """SimpleHTTPRequestHandler extended to honour Range headers."""

    log_message = lambda self, fmt, *a: None  # silence per-request noise

    def do_GET(self):
        path = self.translate_path(self.path)

        # Fall back to parent for directories and non-files.
        if os.path.isdir(path):
            super().do_GET()
            return

        try:
            f = open(path, "rb")
        except OSError:
            self.send_error(404, "File not found")
            return

        try:
            total = os.fstat(f.fileno()).st_size
            range_header = self.headers.get("Range", "")

            if range_header.startswith("bytes="):
                # Parse first range only (browsers send exactly one range).
                spec = range_header[len("bytes="):].split(",")[0].strip()
                start_s, _, end_s = spec.partition("-")
                start = int(start_s) if start_s else 0
                end   = (int(end_s) if end_s else total - 1)
                end   = min(end, total - 1)

                if start > end or start >= total:
                    self.send_response(416, "Range Not Satisfiable")
                    self.send_header("Content-Range", f"bytes */{total}")
                    self.end_headers()
                    return

                length = end - start + 1
                f.seek(start)

                self.send_response(206)
                self.send_header("Content-Type",
                                 self.guess_type(path) or "application/octet-stream")
                self.send_header("Content-Range",
                                 f"bytes {start}-{end}/{total}")
                self.send_header("Content-Length", str(length))
                self.send_header("Accept-Ranges", "bytes")
                self.end_headers()
                self._copy_bytes(f, length)
            else:
                # No Range: return full file with Accept-Ranges header.
                ctype = self.guess_type(path) or "application/octet-stream"
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(total))
                self.send_header("Accept-Ranges", "bytes")
                self.end_headers()
                self._copy_bytes(f, total)
        finally:
            f.close()

    def _copy_bytes(self, f, n):
        buf = 65536
        remaining = n
        while remaining > 0:
            chunk = f.read(min(buf, remaining))
            if not chunk:
                break
            self.wfile.write(chunk)
            remaining -= len(chunk)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8889
    directory = sys.argv[2] if len(sys.argv) > 2 else os.getcwd()
    os.chdir(directory)

    # Bind localhost only.
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", port), RangeHTTPRequestHandler) as httpd:
        print(f"Serving {directory} on http://127.0.0.1:{port}/  (206-capable)")
        sys.stdout.flush()
        httpd.serve_forever()


if __name__ == "__main__":
    main()
