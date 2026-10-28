# Measure vertical extents: red digits band and the face in both shots.
from PIL import Image

def vext(img, x0, x1, y0, y1, test):
    px = img.load()
    ys = [y for y in range(y0, y1) if any(test(px[x, y][:3]) for x in range(x0, x1))]
    return (min(ys), max(ys)) if ys else None

red = lambda c: c[0] > 150 and c[1] < 90 and c[2] < 90

cur = Image.open(r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn\screenshots 2\current\Screenshot 2026-09-06 180201.png").convert("RGB")
ref = Image.open(r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn\screenshots 2\current\6-9-2026\Screenshot 2026-09-06 071530.png").convert("RGB")

# CURRENT: ammo digits img 73..117 (from prior scan, band y 942-1099)
print("cur ammo vext:", vext(cur, 73, 117, 942, 1099, red))
print("cur health vext:", vext(cur, 222, 366, 942, 1099, red))

# REFERENCE: ammo digits img 530..697, band from ~1470
print("ref ammo vext:", vext(ref, 530, 697, 1460, 1749, red))
print("ref health vext:", ref and vext(ref, 774, 1122, 1460, 1749, red))
print("ref armor vext:", vext(ref, 1868, 2035, 1460, 1749, red))

# face: skin-tone pixels near the center of the bar
skin = lambda c: c[0] > 120 and 60 < c[1] < 160 and c[2] < 110 and c[0] > c[1] > c[2]
px = ref.load()
xs = [x for x in range(1300, 1700) if any(skin(px[x, y][:3]) for y in range(1480, 1740))]
print("ref face x-extent:", (min(xs), max(xs)) if xs else None)
ys = [y for y in range(1460, 1749) if any(skin(px[x, y][:3]) for x in range(1300, 1700))]
print("ref face y-extent:", (min(ys), max(ys)) if ys else None)