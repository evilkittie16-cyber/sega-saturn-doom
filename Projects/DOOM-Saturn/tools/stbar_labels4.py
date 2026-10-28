# Zoom in: print per-column palette indices for the AMMO label rows to
# identify the glyph color, then cluster on it across the whole bar.
import struct, os, collections

ROOT = r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn"
f = open(os.path.join(ROOT, "cd", "DOOM.WAD"), "rb")
magic, numlumps, diroffs = struct.unpack("<4sii", f.read(12))
f.seek(diroffs)
dir_ = []
for i in range(numlumps):
    off, ln, nm = struct.unpack("<ii8s", f.read(16))
    dir_.append((nm.rstrip(b"\x00").decode(), off, ln))
for nm, off, ln in dir_:
    if nm == "STBAR":
        f.seek(off); d = f.read(ln); break
    if nm == "PLAYPAL":
        f.seek(off); pal = f.read(768)

w, h = struct.unpack("<hh", d[0:4])
colofs = struct.unpack("<%di" % w, d[8:8 + 4 * w])
px = [[0] * h for _ in range(w)]
for x in range(w):
    p = colofs[x]
    while True:
        top = d[p]
        if top == 0xFF: break
        ln2 = d[p + 1]
        for j in range(ln2):
            px[x][top + j] = d[p + 3 + j]
        p += 4 + ln2

# rows 24-29 carry the label text; histogram that exact band
c = collections.Counter()
for x in range(w):
    for y in range(23, 31):
        c[px[x][y]] += 1
print("label-band hist:", c.most_common(12))

# glyph = brightest frequent index; cluster on indices >= that brightness
# sort candidate indices by brightness
bright = sorted(c.keys(), key=lambda i: -(pal[i*3] + pal[i*3+1] + pal[i*3+2]))
print("brightest in band:", [(i, pal[i*3:i*3+3]) for i in bright[:6]])