# dump STBAR (and related ST lumps) from cd/DOOM.WAD to PNGs for visual check
import struct, sys, os
from zlib import crc32  # noqa

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WAD  = os.path.join(ROOT, "cd", "DOOM.WAD")
OUT  = os.path.dirname(os.path.abspath(__file__))

# PLAYPAL
pal = None

def read_palette(f, dir_):
    for name, off, ln in dir_:
        if name == "PLAYPAL":
            f.seek(off)
            d = f.read(ln)
            return [tuple(d[i*3:i*3+3]) for i in range(256)]
    return None

def write_png(path, w, h, px, pal):
    # px: list of palette indices, row-major
    raw = b""
    for y in range(h):
        raw += b"\x00" + bytes(b for i in range(w) for b in pal[px[y*w+i]])
    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", crc32(c) & 0xffffffff)
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    import zlib as _z
    idat = chunk(b"IDAT", _z.compress(raw, 9))
    iend = chunk(b"IEND", b"")
    with open(path, "wb") as o:
        o.write(sig + ihdr + idat + iend)

def main():
    f = open(WAD, "rb")
    magic, numlumps, diroffs = struct.unpack("<4sii", f.read(12))
    print("magic=%r lumps=%d diroffs=%d size=%d" % (magic, numlumps, diroffs, os.path.getsize(WAD)))
    f.seek(diroffs)
    dir_ = []
    for i in range(numlumps):
        off, ln, nm = struct.unpack("<ii8s", f.read(16))
        nm = nm.rstrip(b"\x00").decode("ascii", "replace")
        dir_.append((nm, off, ln))
    st = [d for d in dir_ if d[0].startswith("ST")]
    for nm, off, ln in st:
        print("%-10s off=%-9d len=%d" % (nm, off, ln))
    p = read_palette(f, dir_)
    # decode STBAR
    for target in ("STBAR",):
        for nm, off, ln in dir_:
            if nm == target:
                f.seek(off)
                d = f.read(ln)
                w, h = struct.unpack("<hh", d[0:4])
                lo, to = struct.unpack("<hh", d[4:8])
                print("%s: %dx%d offs=(%d,%d) posts=%d" % (nm, w, h, lo, to, struct.unpack("<i", d[8:12])[0]))
                # column posts: col offset table at +8 (w ints)
                colofs = struct.unpack("<%di" % w, d[8:8+4*w])
                px = [0]*(w*h)
                for x in range(w):
                    p_ = colofs[x]
                    while True:
                        top = d[p_]
                        if top == 0xFF:
                            break
                        ln2 = d[p_+1]
                        # d32xr/patch layout check: try classic (top, len, pad, px.., pad2)
                        for j in range(ln2):
                            px[(top+j)*w + x] = d[p_+3+j]
                        p_ += 4 + ln2
                write_png(os.path.join(OUT, "stbar.png"), w, h, px, p)
                print("wrote stbar.png")
    f.close()

main()