# Our STBAR at 4x with a 20-px game ruler, plus composite previews with
# numbers drawn at candidate right-edges so we can pick the layout that
# matches the KEX reference.
from PIL import Image, ImageDraw

ROOT = r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn"
st = Image.open(ROOT + r"\tools\stbar.png").convert("RGB")

S = 4
big = Image.new("RGB", (320 * S, (32 + 14) * S), (0, 0, 0))
big.paste(st.resize((320 * S, 32 * S), Image.NEAREST), (0, 0))
d = ImageDraw.Draw(big)
for x in range(0, 321, 20):
    d.line([(x * S, 32 * S), (x * S, (32 + 6) * S)], fill=(255, 255, 0), width=1)
    if x % 40 == 0:
        d.line([(x * S, 0), (x * S, 32 * S)], fill=(0, 255, 255), width=1)
        d.text((x * S + 2, (32 + 7) * S), str(x), fill=(255, 255, 0))
big.save(ROOT + r"\tools\stbar_ruler.png")

# Load digit art (STTNUM0-9, STTMINUS, STTPRCNT) from the WAD and draw
# candidate layouts onto the bar.
import struct

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

def draw_num(img, right, y, value, pct):
    s = "%d" % value
    glyphs = [NUMS[int(c)] for c in s]
    if pct:
        glyphs.append(PCT)
    x = right - sum(g.width for g in glyphs)
    po = img.load()
    for g in glyphs:
        gp = g.load()
        for gy in range(g.height):
            for gx in range(g.width):
                c = gp[gx, gy]
                if c != (0, 0, 0):
                    po[x + gx, y + gy] = c
        x += g.width

def composite(ammo_r, health_r, armor_r, tag):
    im = st.copy()
    draw_num(im, ammo_r, 3, 50, False)
    draw_num(im, health_r, 3, 100, True)
    draw_num(im, armor_r, 3, 0, True)
    im.resize((640, 64), Image.NEAREST).save(ROOT + r"\tools\stbar_cand_%s.png" % tag)

composite(44, 90, 221, "vanilla")
composite(73, 118, 213, "kex")
print("saved")