# Crop the reference's status bar, normalize to 320x32, save for viewing.
# Also crop the current shot's bar the same way.
from PIL import Image

ref = Image.open(r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn\screenshots 2\current\6-9-2026\Screenshot 2026-09-06 071530.png").convert("RGB")
cur = Image.open(r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn\screenshots 2\current\Screenshot 2026-09-06 180201.png").convert("RGB")

out = r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn\tools"

# reference: bar band y 1470..1749, x 0..3054
rbar = ref.crop((0, 1470, 3054, 1749)).resize((320, 32), Image.NEAREST)
rbar.save(out + r"\ref_bar_320.png")

# reference bar at 2x for readability
rbar.resize((640, 64), Image.NEAREST).save(out + r"\ref_bar_640.png")

# current: Kronos window 1332x1099; game 320x224 -> bar bottom 157 px
cbar = cur.crop((0, 1099 - 157, 1332, 1099)).resize((320, 32), Image.NEAREST)
cbar.resize((640, 64), Image.NEAREST).save(out + r"\cur_bar_640.png")
print("saved")