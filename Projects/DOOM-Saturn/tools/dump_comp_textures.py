import struct

with open("cd/DOOM.WAD", "rb") as f:
    wad = f.read()
numlumps, diroff = struct.unpack_from("<II", wad, 4)

def get_lump(name):
    for i in range(numlumps):
        p, s, n = struct.unpack_from("<II8s", wad, diroff + i * 16)
        if n.split(b"\0")[0].decode("ascii") == name:
            return wad[p:p + s]
    return None

pnames_lump = get_lump("PNAMES")
num_pnames = struct.unpack_from("<I", pnames_lump, 0)[0]
pnames = [pnames_lump[4 + i * 8 : 4 + (i + 1) * 8].split(b"\0")[0].decode("latin1") for i in range(num_pnames)]

tex1 = get_lump("TEXTURE1")
num_tex = struct.unpack_from("<I", tex1, 0)[0]
for i in range(num_tex):
    ofs = struct.unpack_from("<I", tex1, 4 + i * 4)[0]
    tname, flags, w, h, _, num_p = struct.unpack_from("<8sHHHHI", tex1, ofs)
    tname = tname.split(b"\0")[0].decode("latin1")
    if any(k in tname for k in ("COMP", "PLAN", "TEK", "LITE", "START")):
        patches = []
        for p in range(num_p):
            px, py, pidx, _, _ = struct.unpack_from("<hhHHH", tex1, ofs + 22 + p * 10)
            patches.append((pnames[pidx], px, py))
        print(f"Texture {tname:8s} {w:3d}x{h:3d} patches={patches}")
