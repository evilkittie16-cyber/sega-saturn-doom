import struct

with open("cd/E1M1.WL", "rb") as f:
    wl = f.read()

magic, ver, num_ssec, total_sz, ofs_ssec, ofs_pil, ofs_segs = struct.unpack_from(">4sHHIIII", wl, 0)
print(f"E1M1.WL: magic={magic} num_ssec={num_ssec} ofs_segs={ofs_segs}")

num_segs = (len(wl) - ofs_segs) // 32
print(f"Total segs in E1M1.WL: {num_segs}")

# Find texture name for COMPUTE2
with open("cd/DOOM.WAD", "rb") as f:
    wad = f.read()

numlumps, diroff = struct.unpack_from("<II", wad, 4)
tex1_p, tex1_s = 0, 0
for i in range(numlumps):
    p, s, n = struct.unpack_from("<II8s", wad, diroff + i * 16)
    name = n.split(b"\0")[0].decode("ascii")
    if name == "TEXTURE1":
        tex1_p, tex1_s = p, s
        break

num_tex = struct.unpack_from("<I", wad, tex1_p)[0]
tex_names = []
for i in range(num_tex):
    t_ofs = struct.unpack_from("<I", wad, tex1_p + 4 + i * 4)[0]
    tname = wad[tex1_p + t_ofs : tex1_p + t_ofs + 8].split(b"\0")[0].decode("latin1").upper()
    tex_names.append(tname)

compute2_idx = tex_names.index("COMPUTE2") if "COMPUTE2" in tex_names else -1
print(f"COMPUTE2 index in TEXTURE1 = {compute2_idx}")

# Check PLANET segs
for i in range(num_segs):
    p0, p1, p2, p3, p4, light, flags, t64, t32, t16, voff, peg, top, bot, u0, uspan, res = struct.unpack_from(">HHHHHBBHHHhHHHHHH", wl, ofs_segs + i * 32)
    if top in (16, 17, 18, 19) or bot in (16, 17, 18, 19):
        is_solid = bool(flags & 0x01)
        is_upper = bool(flags & 0x02)
        is_lower = bool(flags & 0x04)
        is_mid   = bool(flags & 0x08)
        tname = tex_names[t64] if t64 < len(tex_names) else str(t64)
        print(f"Seg {i:4d}: flags=0x{flags:02x} (solid={is_solid}, upper={is_upper}, lower={is_lower}, mid={is_mid}) tex={tname} top_sec={top} bot_sec={bot} peg_sec={peg} voff={voff} uspan={uspan}")
