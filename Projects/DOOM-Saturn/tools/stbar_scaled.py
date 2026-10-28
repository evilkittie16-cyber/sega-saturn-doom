# Preview: 3/4-scaled tall numbers, right-aligned at 39/96/225, ARMS at 146.
from PIL import Image
import struct

ROOT = r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn"
st = Image.open(ROOT + r"\tools\stbar.png").convert("RGB")

f = open(ROOT + r"\cd\DOOM.WAD", "rb")
magic, numlumps, diroffs = struct.unpack("<4sii", f.read(12))
f.seek(diroffs)
dir_ = []
for i in range(numlumps):
    off, ln, nm = struct.unpack("<ii8s", f.read(16))
    dir_.append((nm.rstrip(b"\x00").decode(), off, ln))
lumps = {}
for nm, off, ln in dir_:
    f.seek(off); lumps[nm] = f.read(ln)
f.close()
pal = lumps["PLAYPAL"]

def decode(name):
    d = lumps[name]
    w, h = struct.unpack("<hh", d[0:4])
    colofs = struct.unpack("<%di" % w, d[8:8 + 4 * w])
    out = Image.new("RGB", (w, h), (0, 0, 0))
    po = out.load()
    for x in range(w):
        p = colofs[x]
        while True:
            top = d[p]
            if top == 0xFF: break
            ln2 = d[p + 1]
            for j in range(ln2):
                i = d[p + 3 + j]
                if i:
                    po[x, top + j] = (pal[i*3], pal[i*3+1], pal[i*3+2])
            p += 4 + ln2
    return out

NUMS = {i: decode("STTNUM%d" % i) for i in range(10)}
PCT  = decode("STTPRCNT")
GRAY = {i: decode("STGNUM%d" % i) for i in range(10)}
YELW = {i: decode("STYSNUM%d" % i) for i in range(10)}
AMMN = {i: decode("AMMNUM%d" % i) for i in range(10)}

def blit_sc(img, g, x, y, n=3, dd=4):
    gp = g.load()
    w, h = g.size
    po = img.load()
    for sx in range(w):
        dx = x + (sx * n) // dd
        for sy in range(h):
            c = gp[sx, sy]
            if c != (0, 0, 0):
                dy = y + (sy * n) // dd
                if 0 <= dx < 320 and 0 <= dy < 32:
                    po[dx, dy] = c

def sw(g, n=3, dd=4):
    return g.size[0] * n // dd

def draw_num_sc(img, right, y, value, pct):
    s = "%d" % value
    gs = [NUMS[int(c)] for c in s]
    if pct: gs.append(PCT)
    x = right - sum(sw(g) for g in gs)
    for g in gs:
        blit_sc(img, g, x, y)
        x += sw(g)

im = st.copy()
draw_num_sc(im, 39, 3, 50, False)
draw_num_sc(im, 96, 3, 100, True)
draw_num_sc(im, 225, 3, 100, True)
for i, wnum in enumerate((2, 3, 4, 5, 6, 7)):
    blit_sc(im, YELW[wnum] if wnum in (2, 3) else GRAY[wnum],
            146 + (i % 3) * 12, 4 + (i // 3) * 10)
for i, (cnt, mx) in enumerate(((50, 200), (4, 50), (0, 50), (0, 300))):
    y = (5, 11, 23, 17)[i]
    # small counts stay full size
    def blit(img, g, x, y):
        po, gp = img.load(), g.load()
        for gy in range(g.height):
            for gx in range(g.width):
                c = gp[gx, gy]
                if c != (0, 0, 0) and 0 <= x+gx < 320 and 0 <= y+gy < 32:
                    po[x+gx, y+gy] = c
    def draw_num(img, right, y, value, glyphs):
        s = "%d" % value
        gs = [glyphs[int(c)] for c in s]
        x = right - sum(g.size[0] for g in gs)
        for g in gs:
            blit(img, g, x, y); x += g.size[0]
    draw_num(im, 288, y, cnt, AMMN)
    draw_num(im, 314, y, mx, GRAY)

im.resize((640, 64), Image.NEAREST).save(ROOT + r"\tools\stbar_scaled_preview.png")
print("saved")