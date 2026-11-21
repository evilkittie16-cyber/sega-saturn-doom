"""Rank sweep views by error, skipping ones whose DOS reference failed
(near-uniform reference pane), and build contact sheets of the worst.
usage: python sweeptriage.py <sweep.log> <bmpdir> <outprefix> [top]
"""
import re, sys, cv2, numpy as np
log, bdir, outp = sys.argv[1:4]
top = int(sys.argv[4]) if len(sys.argv) > 4 else 24
rows = []
for l in open(log):
    m = re.match(r'\[SWEEP\] (\S+) E(\d)M(\d) .*AvgErr:\s*([\d.]+)%.*>=1%:\s*([\d.]+)', l)
    if not m:
        continue
    tag, err = m.group(1), float(m.group(4))
    im = cv2.imread('%s/%s.bmp' % (bdir, tag))
    if im is None:
        continue
    sat = im[18:180, 16:336].astype(np.float32)
    ref = im[18:180, 352 + 16:352 + 336].astype(np.float32)
    rstd = float(ref.std())
    rows.append((err, tag, rstd, float(sat.std())))
valid = [r for r in rows if r[2] > 12]
bad = [r for r in rows if r[2] <= 12]
print('views %d  ref-invalid %d' % (len(rows), len(bad)))
valid.sort(reverse=True)
for r in valid[:top]:
    print('%6.2f %-22s refstd %5.1f satstd %5.1f' % r)
tiles = []
for r in valid[:top]:
    im = cv2.imread('%s/%s.bmp' % (bdir, r[1]))[0:184, 0:704]
    cv2.putText(im, '%s %.1f' % (r[1], r[0]), (4, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 255, 255), 1)
    tiles.append(im)
for k in range(0, len(tiles), 6):
    cv2.imwrite('%s_%d.png' % (outp, k // 6), np.vstack(tiles[k:k + 6]))
