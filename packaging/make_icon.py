"""Build packaging/Hamsterball.icns from the game's Hammy1.png texture (the hamster in its ball).
Usage: python3 packaging/make_icon.py [game-dir]   (needs Pillow; only needed to regenerate the icon)"""
import os
import shutil
import subprocess
import sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
game = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "game")
src = Image.open(os.path.join(game, "Textures", "Hammy1.png")).convert("RGBA")
ball = src.crop(src.getbbox())
side = max(ball.size)
art = Image.new("RGBA", (side, side))
art.alpha_composite(ball, ((side - ball.size[0]) // 2, (side - ball.size[1]) // 2))

iconset = os.path.join(HERE, "Hamsterball.iconset")
shutil.rmtree(iconset, ignore_errors=True)
os.makedirs(iconset)
for s in [16, 32, 128, 256, 512]:
    for scale in [1, 2]:
        px = s * scale
        canvas = Image.new("RGBA", (px, px))
        a = art.resize((int(px * 0.84), int(px * 0.84)), Image.LANCZOS)  # macOS icon grid margin
        canvas.alpha_composite(a, ((px - a.size[0]) // 2, (px - a.size[1]) // 2))
        canvas.save(os.path.join(iconset, "icon_%dx%d%s.png" % (s, s, "@2x" if scale == 2 else "")))
subprocess.check_call(["iconutil", "-c", "icns", iconset, "-o", os.path.join(HERE, "Hamsterball.icns")])
shutil.rmtree(iconset)
print("wrote packaging/Hamsterball.icns")
