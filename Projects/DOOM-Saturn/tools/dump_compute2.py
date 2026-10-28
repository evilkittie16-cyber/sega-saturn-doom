import struct
from PIL import Image

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

comp2_patches = []
w, h = 0, 0
for i in range(num_tex):
    ofs = struct.unpack_from("<I", tex1, 4 + i * 4)[0]
    tname, masked, tw, th, _, num_p = struct.unpack_from("<8siHHih", tex1, ofs)
    tname = tname.split(b"\0")[0].decode("latin1").upper()
    if tname == "PLANET1":
        w, h = tw, th
        for p in range(num_p):
            px, py, pidx, _, _ = struct.unpack_from("<hhHHH", tex1, ofs + 22 + p * 10)
            comp2_patches.append((pnames[pidx], px, py))
        break

print(f"COMPUTE2 {w}x{h} patches: {comp2_patches}")

playpal = get_lump("PLAYPAL")[:768]
palette = [playpal[i:i+3] for i in range(0, 768, 3)]

img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
for pname, ox, oy in comp2_patches:
    patch_data = get_lump(pname)
    pw, ph, lox, loy = struct.unpack_from("<hhhh", patch_data, 0)
    for col in range(pw):
        cofs = struct.unpack_from("<I", patch_data, 8 + col * 4)[0]
        while True:
            top = patch_data[cofs]
            if top == 0xFF: break
            length = patch_data[cofs + 1]
            for k in range(length):
                pix = patch_data[cofs + 3 + k]
                rgb = palette[pix]
                rx = ox + col
                ry = oy + top + k
                if 0 <= rx < w and 0 <= ry < h:
                    img.putpixel((rx, ry), (rgb[0], rgb[1], rgb[2], 255))
            cofs += 4 + length

img.save("tools/compute2_ref.png")
print("Saved tools/compute2_ref.png")
