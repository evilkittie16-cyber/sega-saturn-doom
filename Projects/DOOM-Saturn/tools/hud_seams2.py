# Panel seams are mid-dark columns; scan mean brightness per column of
# the metal band and print minima (candidate dividers).
import struct

ROOT = r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn"
f = open(ROOT + r"\cd\DOOM.WAD", "rb")
magic, numlumps, diroffs = struct.unpack("<4sii", f.read(12))
f.seek(diroffs)
dir_ = []
for i in range(numlumps):
    off, ln, nm = struct.unpack("<ii8s", f.read(16))
    dir_.append((nm.rstrip(b"\x00").decode(), off, ln))
for nm, off, ln in dir_:
    if nm == "STBAR":
        f.seek(off); d = f.read(ln); break
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

mean = [sum(px[x][2:20]) / 18.0 for x in range(w)]
# print columns notably darker than both neighbors 3 px away
for x in range(3, w - 3):
    m = mean[x]
    if m < 55 and mean[x - 3] > m + 18 and mean[x + 3] > m + 18:
        print("seam col %d mean %.0f" % (x, m))