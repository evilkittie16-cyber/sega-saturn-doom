import struct,sys
pak,pvs=sys.argv[1],sys.argv[2]
b=bytearray(open(pak,'rb').read()); new=open(pvs,'rb').read()
ns=struct.unpack('>H',b[6:8])[0]
for k in range(ns):
    e=b[16+16*k:32+16*k]
    if e[:4]==b'PVS ':
        off,size=struct.unpack('>II',e[4:12])
        assert size==len(new), (size,len(new))
        assert b[off*2048:off*2048+16]==new[:16]
        b[off*2048:off*2048+size]=new
        open(pak,'wb').write(b); print('swapped PVS in',pak,size,'bytes'); break
