"""Install (or remove) this OptiScaler build into a game folder, beside an existing ReShade.

Adds four files and touches nothing else:
  winmm.dll              OptiScaler (loaded through the game's own winmm import; ReShade keeps dxgi.dll)
  OptiScaler.ini         the shipped ini with a few keys set (see SETTINGS)
  nvngx.dll_dlssnr.dll   the forwarder, built with this solution
  nvngx_dlssnr.dll       NVIDIA's Neural Rendering model -- supplied by you (--model)

Refuses to overwrite a file it did not put there. Uninstall removes exactly those four.

  python deploy/install.py install   --game "<game>/bin/x64_dx12" --model "<path>/nvngx_dlssnr.dll"
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
    "DlssNr": {"Enabled": "true", "CacheEnabled": "true", "CacheInterval": "2"},
}


def configured_ini(template: str) -> str:
    out = []
    section = None
    for line in template.splitlines():
        m = re.match(r"^\[(.+)\]\s*$", line)
        if m:
            section = m.group(1)
        else:
            kv = re.match(r"^([A-Za-z0-9_]+)=", line)
            if kv and section in SETTINGS and kv.group(1) in SETTINGS[section]:
                line = f"{kv.group(1)}={SETTINGS[section][kv.group(1)]}"
        out.append(line)
    return "\r\n".join(out) + "\r\n"


def sha(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def install(game: str, model: str, overwrite_own: bool):
    marker = os.path.join(game, MARKER)
    previous = json.load(open(marker)) if os.path.exists(marker) else {}

    sources = {
        "winmm.dll": os.path.join(ROOT, "x64", "Release", "OptiScaler.dll"),
        "nvngx.dll_dlssnr.dll": os.path.join(ROOT, "x64", "Release", "a", "nvngx.dll_dlssnr.dll"),
        "nvngx_dlssnr.dll": model,
    }

    for name, src in sources.items():
        if not os.path.isfile(src):
            sys.exit(f"missing {src}")

    targets = list(sources) + ["OptiScaler.ini"]

    for name in targets:
        dst = os.path.join(game, name)
        if os.path.exists(dst) and name not in previous:
            sys.exit(f"{dst} already exists and was not installed by this script -- not overwriting it")

    record = {}
    for name, src in sources.items():
        dst = os.path.join(game, name)
        shutil.copy2(src, dst)
        record[name] = sha(dst)
        print(f"  {name:<22} <- {src}")

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

    with open(marker, "w") as f:
        json.dump(record, f, indent=2)
    print(f"installed into {game}")


def uninstall(game: str):
    marker = os.path.join(game, MARKER)
    if not os.path.exists(marker):
        sys.exit("nothing installed by this script here")
    record = json.load(open(marker))
    for name in record:
        p = os.path.join(game, name)
        if os.path.exists(p):
            os.remove(p)
            print(f"  removed {name}")
    os.remove(marker)
    print("uninstalled")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["install", "uninstall"])
    ap.add_argument("--game", required=True, help="folder holding the game's executable")
    ap.add_argument("--model", help="nvngx_dlssnr.dll to install (install only)")
    ap.add_argument("--reset-ini", action="store_true", help="rewrite OptiScaler.ini even if already installed")
    args = ap.parse_args()

    if args.action == "install":
        if not args.model:
            sys.exit("--model is required to install")
        install(args.game, args.model, args.reset_ini)
    else:
        uninstall(args.game)


if __name__ == "__main__":
    main()
