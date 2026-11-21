"""lodgrid.py <set_dir> <out.png> <setting> [<setting> ...]
One row per view: the named settings side by side, then a diff mask of the
last setting vs the first (red = changed pixel, rest dimmed). Rows 0..199."""
import os, sys, struct, zlib

W, H = 320, 200

def read_ppm(p):
    b = open(p, 'rb').read()
    parts, i = [], 0
    while len(parts) < 4:
        while b[i:i+1].isspace(): i += 1
        if b[i:i+1] == b'#':
            while b[i:i+1] != b'\n': i += 1
            continue
        j = i
        while not b[j:j+1].isspace(): j += 1
        parts.append(b[i:j]); i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    px = b[i:i + w * h * 3]
    return [px[y * w * 3:(y * w + W) * 3] for y in range(H)]

def write_png(p, rows, w):
    raw = b''.join(b'\0' + bytes(r) for r in rows)
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(p, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, len(rows), 8, 2, 0, 0, 0)) +
                        chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

d, out, names = sys.argv[1], sys.argv[2], sys.argv[3:]
views = sorted(v for v in os.listdir(d) if os.path.isdir(os.path.join(d, v)))
outrows = []
for v in views:
    ims = [read_ppm(os.path.join(d, v, 'view_%s.ppm' % n)) for n in names]
    for y in range(H):
        a, z = ims[0][y], ims[-1][y]
        m = bytearray(W * 3)
        for x in range(W):
            k = x * 3
            if a[k:k+3] != z[k:k+3]:
                m[k:k+3] = b'\xff\x00\x00'
            else:
                m[k], m[k+1], m[k+2] = z[k] // 3, z[k+1] // 3, z[k+2] // 3
        outrows.append(b''.join(im[y] for im in ims) + bytes(m))
write_png(out, outrows, W * (len(names) + 1))
print(out, views, names)
