"""Write the Android launcher icons (res/mipmap-*/ic_launcher.png) from the game's Hammy1.png.
Usage: python3 android/make_icons.py   (needs Pillow; only needed to regenerate the icons)"""
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
src = Image.open(os.path.join(HERE, "..", "game", "Textures", "Hammy1.png")).convert("RGBA")
ball = src.crop(src.getbbox())
side = max(ball.size)
art = Image.new("RGBA", (side, side))
art.alpha_composite(ball, ((side - ball.size[0]) // 2, (side - ball.size[1]) // 2))
for density, px in {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}.items():
    out = os.path.join(HERE, "app", "src", "main", "res", "mipmap-" + density)
    os.makedirs(out, exist_ok=True)
    canvas = Image.new("RGBA", (px, px))
    a = art.resize((int(px * 0.92), int(px * 0.92)), Image.LANCZOS)
    canvas.alpha_composite(a, ((px - a.size[0]) // 2, (px - a.size[1]) // 2))
    canvas.save(os.path.join(out, "ic_launcher.png"))
print("wrote launcher icons")
