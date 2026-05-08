"""Off-device decode test for the partial-screenshot wire format.

Builds a synthetic MMSCRN payload representing a 100x60 clipped region
(tightly packed, stride = w * 2), zlib-compresses, and runs it through
the same struct.unpack_from + zlib.decompress path used by
tools/fetch_screenshot.py. Asserts dims, stride, format, and a sentinel
pixel come back intact.
"""
import struct
import zlib

W, H   = 100, 60
STRIDE = W * 2
FMT    = 0x12   # LV_COLOR_FORMAT_RGB565

# Build a recognisable RGB565 pattern: each pixel encodes (x ^ y) & 0xFFFF.
pixels = bytearray(W * H * 2)
for y in range(H):
    for x in range(W):
        v = ((x ^ y) * 17 + y) & 0xFFFF
        off = y * STRIDE + x * 2
        pixels[off]     = v & 0xFF
        pixels[off + 1] = (v >> 8) & 0xFF
pixels = bytes(pixels)

ulen = len(pixels)
comp = zlib.compress(pixels)
clen = len(comp)

# Header layout matches the packed C struct in cmd_screenshot_impl.
hdr = struct.pack("<8sIIIIIII",
                  b"MMSCRN\0\0", W, H, STRIDE, FMT, ulen, clen, 1)

payload = hdr + comp

# Replicate the decoder's parse path.
assert len(payload) >= 36, "payload too small"
magic, w, h, stride, fmt, u, c, flags = struct.unpack_from(
    "<8sIIIIIII", payload, 0)
assert magic.startswith(b"MMSCRN"), f"bad magic {magic!r}"
assert w == W and h == H, f"dims {w}x{h} != {W}x{H}"
assert stride == STRIDE, f"stride {stride} != {STRIDE}"
assert fmt == FMT, f"fmt 0x{fmt:02x} != 0x{FMT:02x}"
assert flags & 1, "expected zlib flag set"

body = payload[36:]
assert len(body) == c, f"body len {len(body)} != header.clen {c}"

decoded = zlib.decompress(body) if (flags & 1) else body
assert len(decoded) == u, f"decoded len {len(decoded)} != header.ulen {u}"
assert decoded == pixels, "pixels mismatch after decompress"

# Spot-check the same RGB565 unpack the decoder does for one pixel.
sx, sy = 37, 41
lo = decoded[sy * stride + sx * 2]
hi = decoded[sy * stride + sx * 2 + 1]
v_back = (hi << 8) | lo
v_want = ((sx ^ sy) * 17 + sy) & 0xFFFF
assert v_back == v_want, f"pixel({sx},{sy}) {v_back:#06x} != {v_want:#06x}"

print(f"OK: {w}x{h} stride={stride} fmt=0x{fmt:02x} "
      f"ulen={u} clen={c} ratio={100.0 * c / u:.1f}%")
print(f"OK: pixel({sx},{sy}) round-tripped 0x{v_back:04x}")
