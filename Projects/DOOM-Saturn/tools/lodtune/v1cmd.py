"""Walk the VDP1 command list in a raw VRAM dump; diff two dumps.

usage: python -I v1cmd.py a.bin [b.bin]
One dump: prints the command list. Two: commands (as multisets of
(ctrl, pmod, colr, srca, size, coords)) only in A / only in B.
"""
import collections
import struct
import sys


def walk(vram):
    cmds = []
    addr, stack, seen = 0, [], 0
    while seen < 6000:
        seen += 1
        w = struct.unpack_from(">16H", vram, addr)
        ctrl, link = w[0], w[1]
        if ctrl & 0x8000:
            break
        jp = (ctrl >> 12) & 7
        cm = ctrl & 0xF
        if jp < 4:   # not skipped
            cmds.append((addr, cm, ctrl, w[2], w[3], w[4], w[5], tuple(
                (v - 0x10000 if v & 0x8000 else v) for v in w[6:14])))
        nxt = addr + 32
        j = jp & 3
        if j == 1:
            nxt = link * 8
        elif j == 2:
            stack.append(addr + 32)
            nxt = link * 8
        elif j == 3 and stack:
            nxt = stack.pop()
        addr = nxt & 0x7FFFF
    return cmds


def key(c):
    return c[1:]


def show(c):
    a, cm, ctrl, pmod, colr, srca, size, xy = c
    return "@%05X cm%X ctrl%04X pmod%04X colr%04X srca%04X(%05X) size%04X(w%d h%d) %s" % (
        a, cm, ctrl, pmod, colr, srca, srca * 8, size, (size >> 8) * 8, size & 0xFF, xy)


a = walk(open(sys.argv[1], "rb").read())
if len(sys.argv) == 2:
    for c in a:
        print(show(c))
    sys.exit()
b = walk(open(sys.argv[2], "rb").read())
ca = collections.Counter(key(c) for c in a)
cb = collections.Counter(key(c) for c in b)
print("A %d cmds, B %d cmds" % (len(a), len(b)))
for name, xs, other in (("only in A", a, cb), ("only in B", b, ca)):
    left = collections.Counter(other)
    out = []
    for c in xs:
        k = key(c)
        if left[k] > 0:
            left[k] -= 1
        else:
            out.append(c)
    print("--- %s: %d" % (name, len(out)))
    for c in out[:60]:
        print(show(c))
