"""contact_sheet.py OUT.png IMG... : tile screenshots (PPM/PNG) into one labelled image."""
import os
import sys
from PIL import Image, ImageDraw
out, files = sys.argv[1], sys.argv[2:]
W, H, cols = 320, 240, 4
rows = (len(files) + cols - 1) // cols
sheet = Image.new("RGB", (W * cols, (H + 16) * rows), "black")
d = ImageDraw.Draw(sheet)
for k, f in enumerate(files):
    x, y = (k % cols) * W, (k // cols) * (H + 16)
    try:
        sheet.paste(Image.open(f).convert("RGB").resize((W, H)), (x, y + 16))
    except Exception:
        pass
    d.text((x + 4, y + 2), os.path.basename(f), fill="white")
sheet.save(out)
