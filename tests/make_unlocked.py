"""Create tests/out/unlocked: a copy of the game dir with every race/arena unlocked."""
import shutil, os, sys
here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, "..", "game")
dst = os.path.join(here, "out", "unlocked")
shutil.rmtree(dst, ignore_errors=True)
shutil.copytree(src, dst)
reg = os.path.join(dst, "registry.txt")
lines = open(reg).read().splitlines() if os.path.exists(reg) else ["[HKCU\\SOFTWARE\\RAPTISOFT\\HAMSTERBALL]"]
names = ["WARMUP", "BEGINNER", "INTERMEDIATE", "DIZZY", "TOWER", "UP", "NEON", "EXPERT", "ODD", "TOOB", "WOBBLY",
         "GLASS", "SKY", "MASTER", "IMPOSSIBLE"]
keep = [l for l in lines if not (l.split("=")[0].endswith("RACE") or l.split("=")[0].endswith("ARENA") or l.startswith("MIRRORTOURNAMENT="))]
for n in names:
    keep += [n + "RACE=3:01", n + "ARENA=3:01"]
keep.append("MIRRORTOURNAMENT=3:01")
open(reg, "w").write("\n".join(keep) + "\n")
print("created", dst)
