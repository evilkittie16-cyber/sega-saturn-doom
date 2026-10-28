import os, glob, csv
from PIL import Image

art_dir = r"C:\Users\charl\.gemini\antigravity\brain\4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4"
win_dir = os.path.dirname(os.path.abspath(__file__)) or "."

# 1. Convert BMPs to PNG
bmps = glob.glob(os.path.join(win_dir, "journey_wp*_triple.bmp"))
for b in sorted(bmps):
    base = os.path.splitext(os.path.basename(b))[0]
    if os.path.isdir(art_dir):
        out_png = os.path.join(art_dir, base + ".png")
        img = Image.open(b)
        img.save(out_png, "PNG")
        print(f"Converted {base}.bmp -> {out_png}")

csv_files = sorted(glob.glob(os.path.join(win_dir, "journey_telemetry_MAP_E1M1_*.csv")))
csv_path = csv_files[-1] if csv_files else os.path.join(win_dir, "journey_telemetry_MAP_E1M1_20261011_115510.csv")
print(f"Analyzing CSV: {csv_path}")
rows = []
with open(csv_path, 'r', newline='') as f:
    reader = csv.DictReader(f)
    for r in reader:
        row = {}
        for k, v in r.items():
            if k is None: continue
            try:
                row[k] = float(v)
            except ValueError:
                row[k] = v
        rows.append(row)

n = len(rows)
if n == 0:
    print("CSV empty!")
    exit(0)

avg_errs = [r['avg_err_pct'] for r in rows]
min_errs = [r['min_err_pct'] for r in rows]
max_errs = [r['max_err_pct'] for r in rows]
lt_01 = [r['pct_lt_01'] for r in rows]
fps_list = [r['sat_fps'] for r in rows]
w_quads = [r['sat_wall_quads'] for r in rows]
f_quads = [r['sat_flat_quads'] for r in rows]
t_quads = [r['sat_total_quads'] for r in rows]
subsecs = [r['sat_subsectors'] for r in rows]
culls = [r['sat_bsp_culls'] for r in rows]
segs_vis = [r['sat_segs_vis'] for r in rows]

print(f"\n=======================================================")
print(f"       FULL-MAP E1M1 FLIGHT TELEMETRY ANALYSIS        ")
print(f"=======================================================")
print(f"Total Frames in Flight:        {n}")
print(f"Full-Level Avg Error:          {sum(avg_errs)/n:.3f}%")
print(f"Full-Level Min Error:          {min(avg_errs):.3f}%")
print(f"Full-Level Max Error:          {max(avg_errs):.3f}%")
print(f"Full-Level Bit-Exact (<0.1%):  {sum(lt_01)/n:.2f}%")
print(f"Average Saturn Render FPS:     {sum(fps_list)/n:.1f} FPS")
print(f"Average Wall Quads:            {sum(w_quads)/n:.1f}")
print(f"Average Flat Quads:            {sum(f_quads)/n:.1f}")
print(f"Average Total Quads:           {sum(t_quads)/n:.1f}")
print(f"Max Quads in any Frame:        {max(t_quads):.0f}")
print(f"Average Subsectors Visited:    {sum(subsecs)/n:.1f}")
print(f"Average BSP Occlusion Culls:   {sum(culls)/n:.1f}")
print(f"Average Segs Visible:          {sum(segs_vis)/n:.1f}")

# 3. Landmark Zone Breakdown
zones = [
    ("Spawn Room (WP 0)", lambda r: 950 <= r['cam_x'] <= 1150 and r['cam_y'] < -3400),
    ("Entry Hallway (WP 1-2)", lambda r: 750 <= r['cam_x'] <= 1150 and -3400 <= r['cam_y'] < -3100),
    ("Armor Secret Pool (WP 3)", lambda r: r['cam_x'] < 700 and r['cam_y'] < -3200),
    ("Toxic Sump Entrance (WP 5)", lambda r: 1300 <= r['cam_x'] <= 1600 and r['cam_y'] < -2950),
    ("Zigzag Acid Catwalk (WP 6)", lambda r: 1600 < r['cam_x'] <= 2100 and -2950 <= r['cam_y'] < -2600),
    ("Computer Control Room (WP 8)", lambda r: 2100 < r['cam_x'] <= 2600 and r['cam_y'] >= -2500),
    ("Courtyard Vista Window (WP 9)", lambda r: 2600 < r['cam_x'] <= 3000 and -3350 <= r['cam_y'] < -2700),
    ("Outdoor Courtyard Lawn (WP 10)", lambda r: r['cam_x'] > 3000 and r['cam_y'] < -3500),
    ("Exit Chamber & Switch (WP 12)", lambda r: 3100 <= r['cam_x'] <= 3350 and -3350 <= r['cam_y'] <= -3100),
]

print(f"\n==========================================================================================")
print(f"                   E1M1 ZONE-BY-ZONE TELEMETRY BREAKDOWN                                  ")
print(f"==========================================================================================")
print(f"{'Zone Landmark':<30} | {'Frames':<6} | {'Avg Err':<8} | {'Min-Max':<12} | {'Bit-Exact':<9} | {'FPS':<5} | {'Quads (W/F/Tot)'}")
print(f"------------------------------------------------------------------------------------------")

for name, pred in zones:
    sub = [r for r in rows if pred(r)]
    if len(sub) > 0:
        sn = len(sub)
        avg_e = sum(r['avg_err_pct'] for r in sub) / sn
        min_e = min(r['avg_err_pct'] for r in sub)
        max_e = max(r['avg_err_pct'] for r in sub)
        exact = sum(r['pct_lt_01'] for r in sub) / sn
        fps = sum(r['sat_fps'] for r in sub) / sn
        w_q = sum(r['sat_wall_quads'] for r in sub) / sn
        f_q = sum(r['sat_flat_quads'] for r in sub) / sn
        tot_q = sum(r['sat_total_quads'] for r in sub) / sn
        print(f"{name:<30} | {sn:<6} | {avg_e:6.3f}% | {min_e:4.2f}-{max_e:4.2f}% | {exact:6.2f}%  | {fps:4.1f} | {w_q:.0f} / {f_q:.0f} / {tot_q:.0f}")

print(f"==========================================================================================")
