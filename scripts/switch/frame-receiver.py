#!/usr/bin/env python3
"""frame-receiver.py (AI-assisted): receiver for the Switch on-demand frame grab.

Listens on TCP 28773. The game (R3+Plus combo) connects to the SAME host its live log
streams to -- the nxlink host when launched with `wifi-test.sh`, or gk_log_host.txt --
and sends, per frame: a 16-byte header 'G','K','F','R' + u32 version(1) + u32 width +
u32 height (little-endian), then width*height*4 bytes of RGBA8 (row-major, top row
first). Each frame is saved as .rgba + .png under the output dir (default
hw-logs/frames/).

Run it on the dev machine BEFORE pressing R3+Plus in game:

    python3 scripts/switch/frame-receiver.py [outdir]

Pure stdlib (hand-rolled zlib PNG encoder, no PIL). Ctrl+C to stop.
"""
import os
import socket
import struct
import sys
import time
import zlib

PORT = 28773
MAGIC = b"GKFR"
MAX_PIXELS = 64 * 1024 * 1024  # sanity bound, far above 1080p


def recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(min(65536, n - len(buf)))
        if not chunk:
            return None
        buf += chunk
    return buf


def write_png(path, w, h, rgba):
    """RGBA8 -> PNG, filter 0 rows, stdlib only."""

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    stride = w * 4
    raw = b"".join(b"\x00" + rgba[y * stride:(y + 1) * stride] for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "hw-logs/frames"
    os.makedirs(outdir, exist_ok=True)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", PORT))
    srv.listen(4)
    print(f"frame-receiver: listening on 0.0.0.0:{PORT}, saving to {outdir}/")
    n = 0
    while True:
        conn, addr = srv.accept()
        try:
            hdr = recv_exact(conn, 16)
            if hdr is None or hdr[:4] != MAGIC:
                what = "closed early" if hdr is None else f"bad magic {hdr[:4]!r}"
                print(f"[{addr[0]}] {what}, ignored")
                continue
            version, w, h = struct.unpack("<III", hdr[4:16])
            if version != 1 or w == 0 or h == 0 or w * h > MAX_PIXELS:
                print(f"[{addr[0]}] bad header fields: v={version} {w}x{h}, ignored")
                continue
            total = w * h * 4
            data = recv_exact(conn, total)
            if data is None:
                print(f"[{addr[0]}] frame {w}x{h} truncated, dropped")
                continue
            n += 1
            base = os.path.join(outdir, f"frame_{time.strftime('%H%M%S')}_{n:03d}_{w}x{h}")
            with open(base + ".rgba", "wb") as f:
                f.write(data)
            write_png(base + ".png", w, h, data)
            print(f"[{addr[0]}] {w}x{h} ({total // 1024} KB) -> {base}.png")
        finally:
            conn.close()


if __name__ == "__main__":
    main()
