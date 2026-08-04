# Generate a small test sprite PNG (radial gradient) using only stdlib.
import struct, zlib, math, os

S = 16
rows = []
for y in range(S):
    row = bytearray([0])
    for x in range(S):
        dx = (x + 0.5) / S * 2 - 1
        dy = (y + 0.5) / S * 2 - 1
        r = math.hypot(dx, dy)
        a = max(0, 1 - r) * 255
        row += bytes((255, 255, 255, int(a)))
    rows.append(bytes(row))

def chunk(tag, data):
    c = tag + data
    return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

ihdr = struct.pack(">IIBBBBB", S, S, 8, 6, 0, 0, 0)  # 8-bit RGBA
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b"")
os.makedirs("sprites", exist_ok=True)
with open("sprites/test.png", "wb") as f:
    f.write(png)
print("wrote sprites/test.png", len(png), "bytes")
