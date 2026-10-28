import struct

wad = open('cd/DOOM.WAD', 'rb').read()
numlumps, infotableofs = struct.unpack_from('<ii', wad, 4)
directory = {}
for i in range(numlumps):
    off = infotableofs + i * 16
    name = wad[off+8:off+16].rstrip(b'\0').decode('latin1')
    if name == 'E1M8':
        for j in range(11):
            o2 = infotableofs + (i+j) * 16
            fpos2, size2 = struct.unpack_from('<ii', wad, o2)
            n2 = wad[o2+8:o2+16].rstrip(b'\0').decode('latin1')
            directory[n2] = (fpos2, size2)
        break

def get_lump(name, fmt, size_per):
    fpos, size = directory[name]
    cnt = size // size_per
    return [struct.unpack_from(fmt, wad, fpos + i * size_per) for i in range(cnt)]

SECTORS = get_lump('SECTORS', '<2h8s8s3h', 26)
SSECTORS = get_lump('SSECTORS', '<2h', 4)
SEGS = get_lump('SEGS', '<6h', 12)
LINEDEFS = get_lump('LINEDEFS', '<7h', 14)
SIDEDEFS = get_lump('SIDEDEFS', '<2h8s8s8sh', 30)
VERTEXES = get_lump('VERTEXES', '<2h', 4)

for ss in [33, 47, 55]:
    n_sg, f_sg = SSECTORS[ss]
    ld = SEGS[f_sg][3]
    side = SEGS[f_sg][4]
    sd = LINEDEFS[ld][5 if side == 0 else 6]
    sec_idx = SIDEDEFS[sd][5]
    sec = SECTORS[sec_idx]
    floor_h, ceil_h, floor_pic, ceil_pic, light, special, tag = sec
    f_name = floor_pic.rstrip(b'\0').decode('ascii', 'ignore')
    c_name = ceil_pic.rstrip(b'\0').decode('ascii', 'ignore')
    print(f'SS {ss}: sec {sec_idx}, floor={floor_h}, ceil={ceil_h}, floor_pic={f_name}, ceil_pic={c_name}, light={light}')
    print(f'   segs count: {n_sg}')
    for i in range(n_sg):
        v1, v2, ang, ldef, s, off = SEGS[f_sg + i]
        p1, p2 = VERTEXES[v1], VERTEXES[v2]
        print(f'     seg {f_sg+i}: v1={v1}{p1}, v2={v2}{p2}, linedef={ldef}, side={s}')
