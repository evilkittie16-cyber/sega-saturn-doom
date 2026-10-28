from PIL import Image

img = Image.open(r"C:\Users\charl\.gemini\antigravity\brain\4f9fcb64-eb07-4aa7-9ef5-d8e413cfb0f4\.user_uploaded\media_1791453849266_d6c2d6fc.png")
crop = img.crop((0, 50, 450, 600))
crop.save("tools/bugged_quad_crop.png")
print("Saved tools/bugged_quad_crop.png")
