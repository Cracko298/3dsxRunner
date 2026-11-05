# Builds the 3DSX Runner for AuroraOS: 3dsxRunner.bin, beside this file.

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILD = HERE / "build"
GEN = BUILD / "gen"
NAME = "3dsxRunner"

SUP = ["start.s", "cheats.c", "memtools.c", "kernel.c", "mmu.c", "ipc.c", "lib.c", "gpu.c", "font.c", "svc_gsp.c", "svc_hid.c",
       "svc_fs.c", "svc_http.c", "svc_dsp.c", "svc_misc.c", "loader.c", "main.c"]
ARCH = ["-march=armv6k", "-mtune=mpcore", "-marm", "-mfloat-abi=soft"]
CFLAGS = ARCH + ["-O2", "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-format",
                 "-ffreestanding", "-fno-tree-loop-distribute-patterns", "-ffunction-sections",
                 "-fdata-sections", "-fno-strict-aliasing"]
APP = ["main.c", "overlay.c", "cheatfile.c", "titlecfg.c", "apps.c", "ui.c", "ui_audio.c", "ui_gain.s", "gcadpcm.c", "menu.c", "splash.c", "display.c", "corelink.c", "fsbridge.c", "httpbridge.c", "glue.c"]

verbose = False


def run(cmd, **kw):
    cmd = [str(c) for c in cmd]
    if verbose:
        print("  $ " + " ".join(cmd))
    r = subprocess.run(cmd, **kw)
    if r.returncode:
        sys.exit(r.returncode)
    return r


def find_aurora(given):
    for p in [given, os.environ.get("AURORA_OS"), HERE.parent / "AuroraOS-main", HERE.parent / "AuroraOS"]:
        if p and (Path(p) / "sdk" / "aurcc.py").is_file():
            return Path(p).resolve()
    sys.exit("AuroraOS was not found: pass --aurora PATH (the repository, with sdk/aurcc.py)")


def supervisor(prefix, opt):
    obj = BUILD / "sup"
    obj.mkdir(parents=True, exist_ok=True)
    objs = []
    for src in SUP:
        o = obj / (Path(src).stem + ".o")
        flags = ARCH if src.endswith(".s") else [f for f in CFLAGS if f != "-O2"] + [f"-O{opt}"]
        run([prefix + "gcc", *flags, "-c", HERE / "arm11" / src, "-o", o])
        objs.append(o)
    elf = BUILD / "sup.elf"
    run([prefix + "gcc", *ARCH, "-T", HERE / "arm11" / "sup.ld", "-nostartfiles", "-nostdlib",
         "-Wl,--gc-sections", "-Wl,--build-id=none", "-Wl,--no-warn-rwx-segments",
         f"-Wl,-Map,{BUILD / 'sup.map'}", *objs, "-lgcc", "-o", elf])
    image = BUILD / "sup.bin"
    run([prefix + "objcopy", "-O", "binary", "--only-section=.text", "--only-section=.data", elf, image])
    syms = {}
    for line in run([prefix + "nm", elf], capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    if syms.get("job_start") != 0x25101000:
        sys.exit("the supervisor's entry is not at 0x25101000")
    print(f"supervisor: {image.stat().st_size:,} bytes of code and data, "
          f"ends at {syms['__sup_end']:#010x} (limit 0x25300000)")
    GEN.mkdir(parents=True, exist_ok=True)
    (GEN / "sup_blob.s").write_text(
        '.section .rodata.sup_blob, "a"\n.balign 16\n.global sup_blob\n.global sup_blob_end\n'
        f'sup_blob:\n.incbin "{image.as_posix()}"\nsup_blob_end:\n', encoding="ascii")


def audio_blob():
    names = ("menu", "move", "select", "back", "success", "error")
    lines = ['.section .rodata.ui_audio, "a"']
    for name in names:
        path = HERE / "assets" / "audio" / (name + ".gcadpcm")
        if not path.is_file():
            sys.exit(f"missing {path}: regenerate with tools/make_ui_audio.py and tools/encode_ui_audio.py")
        lines += [".balign 4", f".global ui_{name}", f".global ui_{name}_end",
                  f'ui_{name}:\n.incbin "{path.as_posix()}"\nui_{name}_end:']
    result = GEN / "ui_audio_blob.s"
    result.write_text("\n".join(lines)+"\n", encoding="ascii")
    return result


def app(aurora, prefix, opt):
    env = dict(os.environ)
    gcc_dir = Path(shutil.which(prefix + "gcc") or "").parent
    if prefix != "arm-none-eabi-" or not shutil.which("arm-none-eabi-gcc"):
        env["PATH"] = str(gcc_dir) + os.pathsep + env.get("PATH", "")  # aurcc.py runs arm-none-eabi-gcc
    run([sys.executable, aurora / "sdk" / "aurcc.py", "build",
         *[HERE / "app" / a for a in APP], GEN / "sup_blob.s", audio_blob(),
         "-I", aurora / "include", "-I", HERE / "arm11",
         "--icon", HERE / f"{NAME}.icon", "--build-dir", BUILD / "sdk",
         "-O", opt, "-o", HERE / f"{NAME}.bin", *(["-v"] if verbose else [])], env=env)


def main():
    global verbose
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--aurora", help="the AuroraOS repository")
    ap.add_argument("--prefix", default="arm-none-eabi-",
                    help="compiler prefix, e.g. /opt/devkitpro/devkitARM/bin/arm-none-eabi-")
    ap.add_argument("--opt", default="3", choices=("2", "3", "s"),
                    help="optimization level for both halves (default 3)")
    ap.add_argument("--sup-only", action="store_true", help="only the supervisor")
    ap.add_argument("-v", action="store_true", help="show each command")
    a = ap.parse_args()
    verbose = a.v
    if not shutil.which(a.prefix + "gcc"):
        sys.exit(f"{a.prefix}gcc was not found: install devkitARM, or pass --prefix")
    supervisor(a.prefix, a.opt)
    if not a.sup_only:
        app(find_aurora(a.aurora), a.prefix, a.opt)


if __name__ == "__main__":
    main()
