#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# starts a prism launcher gtnh instance straight from java, without the launcher, just long enough
# for texture-dump's mod to write texture-dump/atlas.png and blocks.json, then the game exits
import json, os, subprocess, sys
from pathlib import Path

PRISM = Path(os.environ.get("PRISM_DIR", Path.home() / ".local/share/PrismLauncher"))
instance = PRISM / "instances" / (sys.argv[1] if len(sys.argv) > 1 else "GT_New_Horizons_2.8.4_Java_17-25")
libs = PRISM / "libraries"
game = instance / ".minecraft"

def allowed(lib):
    rules = lib.get("rules")
    if not rules:
        return True
    ok = False
    for r in rules:
        if "os" not in r or r["os"].get("name") == "linux":
            ok = r["action"] == "allow"
    return ok

def maven_path(name):
    parts = name.split(":")
    group, artifact, version = parts[:3]
    classifier = "-" + parts[3] if len(parts) > 3 else ""
    return f"{group.replace('.', '/')}/{artifact}/{version}/{artifact}-{version}{classifier}.jar"

def lib_file(lib):
    if lib.get("MMC-hint") == "local":
        return instance / "libraries" / Path(maven_path(lib["name"])).name
    # prism stores by maven name, even when the download url is laid out differently
    f = libs / maven_path(lib["name"])
    art = lib.get("downloads", {}).get("artifact")
    if not f.exists() and art and "libraries.minecraft.net/" in art.get("url", ""):
        f = libs / art["url"].split("libraries.minecraft.net/", 1)[1]
    return f

classpath, slot, jvm_args, tweakers = [], {}, [], []
main_class = main_jar = None
for comp in json.load(open(instance / "mmc-pack.json"))["components"]:
    patch = json.load(open(instance / "patches" / f"{comp['uid']}.json"))
    for lib in patch.get("libraries", []) + patch.get("+libraries", []):
        if not allowed(lib):
            continue
        # legacy lwjgl2 natives, lwjgl3ify replaces lwjgl2 entirely
        if "natives" in lib:
            continue
        f = lib_file(lib)
        if not f.exists():
            print(f"missing library {f}", file=sys.stderr)
            continue
        # same artifact from a later component (forge's guava over vanilla's) replaces it in place, like prism
        key = ":".join(lib["name"].split(":")[:2] + lib["name"].split(":")[3:])
        if key in slot:
            classpath[slot[key]] = str(f)
        else:
            slot[key] = len(classpath)
            classpath.append(str(f))
    jvm_args += patch.get("+jvmArgs", [])
    tweakers += patch.get("+tweakers", [])
    main_class = patch.get("mainClass", main_class)
    if "mainJar" in patch:
        main_jar = libs / maven_path(patch["mainJar"]["name"])
classpath.append(str(main_jar))

cmd = ["java", "-Xmx12G", "-Dtexturedump.exit=true", *jvm_args, "-cp", ":".join(classpath), main_class,
       "--username", "Mapper", "--version", "1.7.10", "--gameDir", str(game),
       "--assetsDir", str(PRISM / "assets"), "--assetIndex", "1.7.10",
       "--uuid", "00000000000000000000000000000000", "--accessToken", "0",
       "--userProperties", "{}", "--userType", "legacy"]
for t in tweakers:
    cmd += ["--tweakClass", t]

# on nixos glfw, libglvnd and x11 aren't on any default path, prism builds the list itself.
# borrow it from a running prism and append it to ours
env = dict(os.environ)
try:
    pid = subprocess.check_output(["pgrep", "-n", "prismlauncher"], text=True).strip()
    for kv in open(f"/proc/{pid}/environ", "rb").read().split(b"\0"):
        if kv.startswith(b"LD_LIBRARY_PATH="):
            env["LD_LIBRARY_PATH"] = ":".join(p for p in [env.get("LD_LIBRARY_PATH"), kv.split(b"=", 1)[1].decode()] if p)
except (subprocess.CalledProcessError, OSError):
    print("no running prism to borrow LD_LIBRARY_PATH from, opengl may not load", file=sys.stderr)

print(f"{len(classpath)} jars, main class {main_class}", file=sys.stderr)
sys.exit(subprocess.call(cmd, cwd=game, env=env))
