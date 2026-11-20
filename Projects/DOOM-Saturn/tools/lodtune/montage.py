"""montage.py <out.png> <a.ppm> [b.ppm ...]  - 2-column grid of PPM frames
(cropped to 320x200 by lodgrid.read_ppm)."""
import os, sys
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'lodgrid.py')).read()
ns = {}
exec(src.split('\nd, out, names')[0], ns)
imgs = [ns['read_ppm'](p) for p in sys.argv[2:]]
blank = bytes(ns['W'] * 3)
rows = []
for g in range(0, len(imgs), 2):
    for y in range(ns['H']):
        rows.append(bytes(imgs[g][y]) + (bytes(imgs[g + 1][y]) if g + 1 < len(imgs) else blank))
ns['write_png'](sys.argv[1], rows, 2 * ns['W'])
