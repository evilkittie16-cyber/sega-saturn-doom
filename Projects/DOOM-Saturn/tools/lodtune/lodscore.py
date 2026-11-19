"""Score a strip-LOD sweep (lodsweep_run.ps1).

usage: python -I lodscore.py <set_dir> [ref_setting=ref] [view_rows=200]

Per view and setting:
  master  ms/frame of master CPU work, minus I_FinishUpdate and the
          slave-wait spin in Saturn_HW_RenderPlayerView (hot loop PCs)
  slave   ms/frame of slave work (minus HW_Slave_FlatWorker_Loop idle)
  quads   VDP1 wall quads (distorted sprites 16/32/64 wide), cmds = all
  psnr    of rows [0, view_rows) vs the reference setting's render (dB;
          inf = identical), chg = % of those pixels that differ
"""
import bisect
import csv
import math
import os
import struct
import sys

CLK = 28636.36


def syms(path):
    out = []
    for line in open(path):
        p = line.split()
        if len(p) >= 3:
            try:
                out.append((int(p[0], 16), p[-1].lstrip("_")))
            except ValueError:
                pass
    out.sort()
    return out


def cpu_ms(prof, sy):
    addrs = [a for a, _ in sy]
    rv = next(a for a, n in sy if n == "Saturn_HW_RenderPlayerView")
    rows = []
    frames = 0
    for r in csv.DictReader(open(prof)):
        pc = int(r["pc"], 16) & 0x07FFFFFF
        fn = sy[bisect.bisect_right(addrs, pc) - 1][1]
        rows.append((r["cpu"], pc, int(r["cycles"]), int(r["count"]), fn))
        if r["cpu"] == "M" and pc == rv:
            frames += int(r["count"])
    frames = max(frames, 1)
    m = s = 0
    for cpu, pc, cyc, cnt, fn in rows:
        if cpu == "M":
            if fn.startswith("I_FinishUpdate"):
                continue
            if fn == "Saturn_HW_RenderPlayerView" and cnt > 100 * frames:
                continue
            m += cyc
        else:
            if fn == "HW_Slave_FlatWorker_Loop":
                continue
            s += cyc
    return m / frames / CLK, s / frames / CLK, frames


def quads(path):
    v = open(path, "rb").read()
    seen, i, wall, cmds = set(), 0, 0, 0
    while i not in seen and cmds < 4000:
        seen.add(i)
        c = v[i * 32:(i + 1) * 32]
        ctrl, link, _, _, _, size = struct.unpack(">6H", c[:12])
        cmds += 1
        if (ctrl & 0xF) == 2 and ((size >> 8) & 0x3F) * 8 in (16, 32, 64):
            wall += 1
        if ctrl & 0x8000:
            break
        jp = (ctrl >> 12) & 7
        i = link // 4 if jp in (1, 2, 5, 6) else i + 1
    return wall, cmds


def ppm(path):
    d = open(path, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def psnr(a, b, w, rows):
    n = w * rows * 3
    se = 0
    chg = 0
    for i in range(0, n, 3):
        d0, d1, d2 = a[i] - b[i], a[i + 1] - b[i + 1], a[i + 2] - b[i + 2]
        if d0 or d1 or d2:
            chg += 1
            se += d0 * d0 + d1 * d1 + d2 * d2
    if se == 0:
        return float("inf"), 0.0
    return 10 * math.log10(255 * 255 / (se / n)), 100.0 * chg / (w * rows)


def main():
    d = sys.argv[1]
    ref = sys.argv[2] if len(sys.argv) > 2 else "ref"
    rows = int(sys.argv[3]) if len(sys.argv) > 3 else 200
    tot = {}
    for view in sorted(os.listdir(d)):
        vd = os.path.join(d, view)
        if not os.path.isfile(os.path.join(vd, "settings.txt")):
            continue
        sy = syms(os.path.join(vd, "syms.txt"))
        names = [l.split()[0] for l in open(os.path.join(vd, "settings.txt"))
                 if l.strip() and not l.startswith("#")]
        w, h, rimg = ppm(os.path.join(vd, "view_%s.ppm" % ref))
        print("\n== %s" % view)
        print("%-14s %7s %7s %6s %5s %7s %6s" % ("setting", "master", "slave", "quads", "cmds", "psnr", "chg%"))
        for n in names:
            try:
                m, s, f = cpu_ms(os.path.join(vd, "prof_%s.csv" % n), sy)
                q, c = quads(os.path.join(vd, "vram_%s.bin" % n))
                _, _, img = ppm(os.path.join(vd, "view_%s.ppm" % n))
                p, ch = psnr(img, rimg, w, rows)
            except (OSError, StopIteration) as e:
                print("%-14s missing (%s)" % (n, e))
                continue
            print("%-14s %7.2f %7.2f %6d %5d %7.2f %6.2f" % (n, m, s, q, c, p, ch))
            t = tot.setdefault(n, [0, 0, 0, 0, 0])
            t[0] += m; t[1] += s; t[2] += q; t[3] += (p if p != float("inf") else 99); t[4] += 1
    print("\n== sum over views (psnr = mean, 99 = identical)")
    for n, t in tot.items():
        print("%-14s master %6.2f slave %6.2f quads %5d psnr %6.2f" % (n, t[0], t[1], t[2], t[3] / t[4]))


if __name__ == "__main__":
    main()
