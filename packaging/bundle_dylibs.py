"""Copy the non-system dylib dependencies of the app's executables into Contents/Frameworks
and point them at @rpath. Usage: bundle_dylibs.py <app> <exe-name>..."""
import os
import shutil
import subprocess
import sys

app = sys.argv[1]
exes = [os.path.join(app, "Contents", "MacOS", n) for n in sys.argv[2:]]
fw = os.path.join(app, "Contents", "Frameworks")
os.makedirs(fw, exist_ok=True)


def deps(path):
    out = subprocess.check_output(["otool", "-L", path], text=True).splitlines()[1:]
    return [l.strip().split(" (")[0] for l in out]


def run(*args):
    subprocess.check_call(args, stderr=subprocess.DEVNULL)


done = {}
queue = list(exes)
while queue:
    f = queue.pop()
    for d in deps(f):
        if d.startswith("/System/") or d.startswith("/usr/lib/") or d.startswith("@"):
            continue
        name = os.path.basename(d)
        if name not in done:
            dst = os.path.join(fw, name)
            shutil.copy(os.path.realpath(d), dst)
            os.chmod(dst, 0o755)
            run("install_name_tool", "-id", "@rpath/" + name, dst)
            done[name] = dst
            queue.append(dst)
        run("install_name_tool", "-change", d, "@rpath/" + name, f)

# sdl2-compat dlopen()s SDL3 from its own directory
if "libSDL2-2.0.0.dylib" in done:
    for cand in ["/opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib", "/usr/local/opt/sdl3/lib/libSDL3.0.dylib"]:
        if os.path.exists(cand):
            dst = os.path.join(fw, "libSDL3.dylib")
            shutil.copy(os.path.realpath(cand), dst)
            os.chmod(dst, 0o755)
            run("install_name_tool", "-id", "@rpath/libSDL3.dylib", dst)
            done["libSDL3.dylib"] = dst
            break
    else:
        sys.exit("SDL3 not found (brew install sdl3)")

for exe in exes:
    subprocess.call(["install_name_tool", "-add_rpath", "@executable_path/../Frameworks", exe], stderr=subprocess.DEVNULL)
print("bundled:", ", ".join(sorted(done)))
