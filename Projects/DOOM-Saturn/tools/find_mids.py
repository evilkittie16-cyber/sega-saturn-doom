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
numlines = len(lines) // 14

for l in range(numlines):
    v1, v2, flags, spec, tag, s1, s2 = struct.unpack_from("<hhhhhhh", lines, l * 14)
    if s2 != -1: # 2-sided linedef
        # check if either side has midtexture
        for s_idx in (s1, s2):
            xoff, yoff, top, bot, mid, sec = struct.unpack_from("<hh8s8s8sh", sides, s_idx * 30)
            mid = mid.split(b"\0")[0].decode("latin1")
            if mid != "-" and mid != "":
                print(f"2-sided line {l:3d}: flags=0x{flags:04x} s_idx={s_idx} mid={mid} sec={sec}")
