# Full corrected-layout preview on our STBAR art:
#   ammo right 39, health right 96, armor right 225 (label right + 2)
#   ARMS box digits start 148 (black box at 145-180)
#   keys 239, counts right 288 / max 314 (unchanged)
from PIL import Image, ImageDraw
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
    f.seek(off)
    lumps[nm] = f.read(ln)
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
            if top == 0xFF:
                break
            ln2 = d[p + 1]
            for j in range(ln2):
                i = d[p + 3 + j]
                if i:
                    po[x, top + j] = (pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2])
            p += 4 + ln2
    return out

NUMS = {i: decode("STTNUM%d" % i) for i in range(10)}
PCT = decode("STTPRCNT")
GRAY = {i: decode("STGNUM%d" % i) for i in range(10)}
YELW = {i: decode("STYSNUM%d" % i) for i in range(10)}
AMMN = {i: decode("AMMNUM%d" % i) for i in range(10)}

def blit(img, g, x, y):
    po, gp = img.load(), g.load()
    for gy in range(g.height):
        for gx in range(g.width):
            c = gp[gx, gy]
            if c != (0, 0, 0):
                if 0 <= x + gx < 320 and 0 <= y + gy < 32:
                    po[x + gx, y + gy] = c

def draw_num(img, right, y, value, pct, glyphs=NUMS, pct_glyph=None):
    s = "%d" % value
    gs = [glyphs[int(c)] for c in s]
    if pct and pct_glyph:
        gs.append(pct_glyph)
    x = right - sum(g.width for g in gs)
    for g in gs:
        blit(img, g, x, y)
        x += g.width

im = st.copy()
draw_num(im, 39, 3, 50, False)                    # ammo
draw_num(im, 96, 3, 100, True)                    # health
draw_num(im, 225, 3, 100, True)                   # armor (demo: 100 to test 3-digit)
# ARMS: 2 3 4 / 5 6 7, yellow owned / gray not
for i, wnum in enumerate((2, 3, 4, 5, 6, 7)):
    x = 148 + (i % 3) * 12
    y = 4 + (i // 3) * 10
    blit(im, YELW[wnum] if wnum in (2, 3) else GRAY[wnum], x, y)
# per-type counts
for i, (cnt, mx) in enumerate(((50, 200), (4, 50), (0, 50), (0, 300))):
    y = (5, 11, 23, 17)[i]
    draw_num(im, 288, y, cnt, False, AMMN)
    draw_num(im, 314, y, mx, False, GRAY)

im.resize((640, 64), Image.NEAREST).save(ROOT + r"\tools\stbar_cand_final.png")
print("saved")