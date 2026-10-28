import struct

with open("cd/DOOM.WAD", "rb") as f:
    wad = f.read()

numlumps, diroff = struct.unpack_from("<II", wad, 4)
dir = []
for i in range(numlumps):
    pos, size, name = struct.unpack_from("<II8s", wad, diroff + i * 16)
    name = name.split(b"\x00")[0].decode("ascii")
    dir.append((name, pos, size))

def get_lump(mapname, lumpname):
    idx = next(i for i, (n, _, _) in enumerate(dir) if n == mapname)
    for i in range(idx, idx + 11):
        if dir[i][0] == lumpname:
            return wad[dir[i][1]:dir[i][1] + dir[i][2]]
    return None

lines = get_lump("E1M1", "LINEDEFS")
sides = get_lump("E1M1", "SIDEDEFS")
verts = get_lump("E1M1", "VERTEXES")
sectors = get_lump("E1M1", "SECTORS")
numlines = len(lines) // 14

def get_v(idx):
    return struct.unpack_from("<hh", verts, idx * 4)

def read_side(s_idx):
    if s_idx == -1: return None
    xoff, yoff, top, bot, mid, sec = struct.unpack_from("<hh8s8s8sh", sides, s_idx * 30)
    return (top.split(b"\0")[0].decode("latin1"), bot.split(b"\0")[0].decode("latin1"), mid.split(b"\0")[0].decode("latin1"), sec)

# Find lines near X=704, Y between -3600 and -3100
for l in range(numlines):
    v1, v2, flags, spec, tag, s1, s2 = struct.unpack_from("<hhhhhhh", lines, l * 14)
    x1, y1 = get_v(v1)
    x2, y2 = get_v(v2)
    if (x1 <= 750 or x2 <= 750) and -3650 <= y1 <= -2900 and -3650 <= y2 <= -2900:
        sd1 = read_side(s1)
        sd2 = read_side(s2)
        print(f"Line {l:3d}: ({x1},{y1})->({x2},{y2}) flags=0x{flags:04x} s1={sd1} s2={sd2}")
