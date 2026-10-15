"""Stage a DLL update for an existing OptiScalerClient installation; never edit the game.

Usage: python deploy/prepare_helldivers2.py --game <bin> --output <new-folder>
The user's NVIDIA NR model and existing FG dependencies are not redistributed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parents[1]
OVERRIDES = {
    "Menu": {"OverlayMenu": "true", "ShortcutKey": "0x2D"},
    "DlssNr": {"Enabled": "true"},
    "Log": {"LogToFile": "true", "LogLevel": "2"},
}


def configure_ini(text: str) -> str:
    """Keep existing FG/NR tuning and comments, supporting the client's spaced INI."""
    settings = {s.lower(): {k.lower(): (k, v) for k, v in values.items()}
                for s, values in OVERRIDES.items()}
    seen_sections, seen_keys = set(), set()
    section = None
    result = []

    def missing_keys():
        for key, (name, value) in settings.get(section, {}).items():
            if (section, key) not in seen_keys:
                result.append(f"{name} = {value}")
                seen_keys.add((section, key))

    for line in text.lstrip("\ufeff").splitlines():
        header = re.match(r"^\s*\[([^\]]+)\]\s*(?:[;#].*)?$", line)
        if header:
            missing_keys()
            section = header.group(1).strip().lower()
            seen_sections.add(section)
        else:
            key = re.match(r"^(\s*)([^;#\s=]+)\s*=", line)
            if key and key.group(2).lower() in settings.get(section, {}):
                normalized = key.group(2).lower()
                _, value = settings[section][normalized]
                line = f"{key.group(1)}{key.group(2)} = {value}"
                seen_keys.add((section, normalized))
        result.append(line)
    missing_keys()
    for name, values in OVERRIDES.items():
        if name.lower() not in seen_sections:
            result.extend(["", f"[{name}]"])
            result.extend(f"{key} = {value}" for key, value in values.items())
    return "\r\n".join(result) + "\r\n"


def stage(game: Path, build: Path, output: Path):
    game, build, output = game.resolve(), build.resolve(), output.resolve()
    if output == game or game in output.parents:
        raise ValueError("Output must be outside the game directory")
    if output.exists():
        raise ValueError("Use a new output folder; existing files are never overwritten")
    inputs = {
        "dxgi.dll": build / "OptiScaler.dll",
        "nvngx.dll_dlssnr.dll": build / "nvngx.dll_dlssnr.dll",
    }
    required = [game / "helldivers2.exe", game / "dxgi.dll", game / "OptiScaler.ini",
                game / "OptiScaler/streamline/sl.interposer.dll",
                game / "OptiScaler/streamline/sl.common.dll",
                game / "OptiScaler/streamline/sl.dlss_g.dll", *inputs.values()]
    missing = [str(p) for p in required if not p.is_file()]
    if missing:
        raise FileNotFoundError("Missing required files:\n" + "\n".join(missing))
    configured = configure_ini((game / "OptiScaler.ini").read_text(encoding="utf-8-sig"))
    output.mkdir(parents=True)
    for name, source in inputs.items():
        shutil.copy2(source, output / name)
    (output / "OptiScaler.ini").write_bytes(configured.encode("utf-8"))
    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
              for p in output.iterdir() if p.is_file()}
    (output / "SHA256.json").write_text(json.dumps(hashes, indent=2), encoding="utf-8")
    print(f"Staged update: {output}")
    print("Game files unchanged. Keep existing FG dependencies and supply nvngx_dlssnr.dll.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", required=True, type=Path)
    parser.add_argument("--build", default=ROOT / "x64/Release/a", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    stage(args.game, args.build, args.output)
