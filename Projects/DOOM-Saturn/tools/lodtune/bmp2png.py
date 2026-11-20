"""bmp2png.py in.bmp out.png [x0 y0 x1 y1] [scale] - 24/32-bit BMP to PNG, optional crop + integer upscale."""
import os, struct, sys
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'lodgrid.py')).read()
ns = {}
exec(src.split('\nd, out, names')[0], ns)
b = open(sys.argv[1], 'rb').read()
off = struct.unpack_from('<I', b, 10)[0]
w, h = struct.unpack_from('<ii', b, 18)
bpp = struct.unpack_from('<H', b, 28)[0] // 8
stride = (w * bpp + 3) & ~3
flip = h > 0; h = abs(h)
rows = []
for y in range(h):
    sy = h - 1 - y if flip else y
    r = b[off + sy * stride: off + sy * stride + w * bpp]
    row = bytearray()
    for x in range(w):
        p = r[x * bpp: x * bpp + 3]
        row += bytes((p[2], p[1], p[0]))
    rows.append(row)
x0, y0, x1, y1 = (0, 0, w, h)
if len(sys.argv) >= 7:
    x0, y0, x1, y1 = map(int, sys.argv[3:7])
s = int(sys.argv[7]) if len(sys.argv) >= 8 else 1
out = []
for y in range(y0, y1):
    row = bytearray()
    for x in range(x0, x1):
        row += rows[y][x * 3:x * 3 + 3] * s
    for _ in range(s):
        out.append(bytes(row))
ns['write_png'](sys.argv[2], out, (x1 - x0) * s)
print(w, h)
