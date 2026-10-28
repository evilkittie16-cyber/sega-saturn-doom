import struct

with open("cd/DOOM.WAD", "rb") as f:
    wad = f.read()
numlumps, diroff = struct.unpack_from("<II", wad, 4)

def get_lump(mapname, lump):
    for i in range(numlumps):
        p, s, n = struct.unpack_from("<II8s", wad, diroff + i * 16)
        if n.split(b"\0")[0].decode("ascii") == mapname:
            for j in range(i, i + 11):
                p2, s2, n2 = struct.unpack_from("<II8s", wad, diroff + j * 16)
                if n2.split(b"\0")[0].decode("ascii") == lump:
                    return wad[p2:p2 + s2]
    return None

lines = get_lump("E1M1", "LINEDEFS")
sides = get_lump("E1M1", "SIDEDEFS")
verts = get_lump("E1M1", "VERTEXES")
secs = get_lump("E1M1", "SECTORS")

def get_v(idx): return struct.unpack_from("<hh", verts, idx * 4)
def read_side(s):
    if s == -1: return None
    xoff, yoff, top, bot, mid, sec = struct.unpack_from("<hh8s8s8sh", sides, s * 30)
    return (top.split(b"\0")[0].decode("latin1").upper(), bot.split(b"\0")[0].decode("latin1").upper(), mid.split(b"\0")[0].decode("latin1").upper(), sec)

def read_sec(s):
    if s == -1: return None
    fh, ch, fpic, cpic, light, spec, tag = struct.unpack_from("<hh8s8shhh", secs, s * 26)
    return (fh, ch, fpic.split(b"\0")[0].decode("latin1"), cpic.split(b"\0")[0].decode("latin1"), light)

numlines = len(lines) // 14
print("Lines with PLANET1 in E1M1:")
for l in range(numlines):
    v1, v2, flags, spec, tag, s1, s2 = struct.unpack_from("<hhhhhhh", lines, l * 14)
    sd1 = read_side(s1)
    sd2 = read_side(s2)
    s1_has = sd1 and any("PLANET" in x for x in sd1[:3])
    s2_has = sd2 and any("PLANET" in x for x in sd2[:3])
    if s1_has or s2_has:
        x1, y1 = get_v(v1)
        x2, y2 = get_v(v2)
        sec1 = read_sec(sd1[3]) if sd1 else None
        sec2 = read_sec(sd2[3]) if sd2 else None
        print(f"Line {l:3d}: ({x1},{y1})->({x2},{y2}) flags=0x{flags:04x} s1={sd1} s2={sd2} sec1={sec1} sec2={sec2}")
