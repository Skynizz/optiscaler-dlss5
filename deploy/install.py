"""Install (or remove) this OptiScaler build into a game folder, beside an existing ReShade.

Adds four files and touches nothing else:
  winmm.dll              OptiScaler (loaded through the game's own winmm import; ReShade keeps dxgi.dll)
                         -- or another name with --name, e.g. OptiScaler.asi beside an ASI loader
  OptiScaler.ini         the shipped ini with a few keys set (see SETTINGS)
  nvngx.dll_dlssnr.dll   the forwarder, built with this solution
  nvngx_dlssnr.dll       NVIDIA's Neural Rendering model -- supplied by you (--model)

Refuses to overwrite a file it did not put there. Uninstall removes exactly those four.

  python deploy/install.py install   --game "<game>/bin/x64_dx12" --model "<path>/nvngx_dlssnr.dll"
          [--name OptiScaler.asi] [--disable renodx-dlss5.addon64]   (disabled files are renamed, and
                                                                       restored by uninstall)
  python deploy/install.py uninstall --game "<game>/bin/x64_dx12"
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARKER = "optiscaler-nr-cache.installed.json"

# section -> {key: value}. Everything else stays at the shipped default (auto).
SETTINGS = {
    "Upscalers": {"Dx12Upscaler": "dlss"},
    "Log": {"LogToFile": "true", "LogLevel": "2"},
    "DlssNr": {"Enabled": "true", "CacheEnabled": "true", "CacheInterval": "2", "CacheSpread": "false",
               "CacheRefreshBlend": "1.0"},
}


def configured_ini(template: str) -> str:
    out = []
    section = None
    written = set()

    def add_missing(sec):
        # Keys the shipped ini does not list (some are read but undocumented there) go at the end of
        # their own section.
        for k, v in SETTINGS.get(sec, {}).items():
            if (sec, k) not in written:
                out.append(f"{k}={v}")
                written.add((sec, k))

    for line in template.splitlines():
        m = re.match(r"^\[(.+)\]\s*$", line)
        if m:
            add_missing(section)
            section = m.group(1)
        else:
            kv = re.match(r"^([A-Za-z0-9_]+)=", line)
            if kv and section in SETTINGS and kv.group(1) in SETTINGS[section]:
                line = f"{kv.group(1)}={SETTINGS[section][kv.group(1)]}"
                written.add((section, kv.group(1)))
        out.append(line)
    add_missing(section)
    return (chr(13) + chr(10)).join(out) + chr(13) + chr(10)


def sha(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def install(game: str, model: str, overwrite_own: bool, name: str = "winmm.dll", disable=()):
    marker = os.path.join(game, MARKER)
    previous = json.load(open(marker)) if os.path.exists(marker) else {}

    # Another mod doing the same job is switched off by renaming, never deleted.
    disabled = previous.get("__disabled__", [])
    for d in disable:
        src = os.path.join(game, d)
        if os.path.exists(src):
            os.replace(src, src + ".disabled")
            disabled.append(d)
            print(f"  {d:<22} disabled (renamed to {d}.disabled)")

    # The model is often already there from another install. Identical: leave it, and leave it behind
    # on uninstall too, since it was not ours.
    model_dst = os.path.join(game, "nvngx_dlssnr.dll")
    keep_model = "nvngx_dlssnr.dll" not in previous and os.path.exists(model_dst) and sha(model_dst) == sha(model)

    sources = {
        name: os.path.join(ROOT, "x64", "Release", "OptiScaler.dll"),
        "nvngx.dll_dlssnr.dll": os.path.join(ROOT, "x64", "Release", "a", "nvngx.dll_dlssnr.dll"),
        "nvngx_dlssnr.dll": model,
    }

    if keep_model:
        del sources["nvngx_dlssnr.dll"]
        print("  nvngx_dlssnr.dll       already there and identical, left as it is")

    for fname, src in sources.items():
        if not os.path.isfile(src):
            sys.exit(f"missing {src}")

    targets = list(sources) + ["OptiScaler.ini"]

    for fname in targets:
        dst = os.path.join(game, fname)
        if os.path.exists(dst) and fname not in previous:
            sys.exit(f"{dst} already exists and was not installed by this script -- not overwriting it")

    record = {}
    for fname, src in sources.items():
        dst = os.path.join(game, fname)
        shutil.copy2(src, dst)
        record[fname] = sha(dst)
        print(f"  {fname:<22} <- {src}")

    ini_dst = os.path.join(game, "OptiScaler.ini")
    if os.path.exists(ini_dst) and not overwrite_own:
        print(f"  OptiScaler.ini         kept (already installed; --reset-ini to rewrite it)")
    else:
        with open(os.path.join(ROOT, "OptiScaler.ini"), "r", encoding="utf-8-sig") as f:
            text = configured_ini(f.read())
        with open(ini_dst, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        print(f"  OptiScaler.ini         written ({', '.join(f'{s}.{k}={v}' for s, kv in SETTINGS.items() for k, v in kv.items())})")
    record["OptiScaler.ini"] = "config"
    record["__disabled__"] = disabled

    with open(marker, "w") as f:
        json.dump(record, f, indent=2)
    print(f"installed into {game}")


def uninstall(game: str):
    marker = os.path.join(game, MARKER)
    if not os.path.exists(marker):
        sys.exit("nothing installed by this script here")
    record = json.load(open(marker))
    for fname in record:
        if fname == "__disabled__":
            continue
        p = os.path.join(game, fname)
        if os.path.exists(p):
            os.remove(p)
            print(f"  removed {fname}")
    for d in record.get("__disabled__", []):
        p = os.path.join(game, d)
        if os.path.exists(p + ".disabled") and not os.path.exists(p):
            os.replace(p + ".disabled", p)
            print(f"  restored {d}")
    os.remove(marker)
    print("uninstalled")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["install", "uninstall"])
    ap.add_argument("--game", required=True, help="folder holding the game's executable")
    ap.add_argument("--model", help="nvngx_dlssnr.dll to install (install only)")
    ap.add_argument("--reset-ini", action="store_true", help="rewrite OptiScaler.ini even if already installed")
    ap.add_argument("--name", default="winmm.dll", help="file name OptiScaler is installed as (default winmm.dll)")
    ap.add_argument("--disable", action="append", default=[], help="a file to switch off by renaming (repeatable)")
    ap.add_argument("--set", action="append", default=[], metavar="SECTION.KEY=VALUE",
                    help="an extra ini setting for this game, e.g. DlssNr.WhitePointSource=0 (repeatable)")
    args = ap.parse_args()

    for item in args.set:
        key, _, value = item.partition("=")
        section, _, name = key.partition(".")
        if not (section and name and value):
            sys.exit(f"--set expects SECTION.KEY=VALUE, got {item}")
        SETTINGS.setdefault(section, {})[name] = value

    if args.action == "install":
        if not args.model:
            sys.exit("--model is required to install")
        install(args.game, args.model, args.reset_ini, args.name, args.disable)
    else:
        uninstall(args.game)


if __name__ == "__main__":
    main()
