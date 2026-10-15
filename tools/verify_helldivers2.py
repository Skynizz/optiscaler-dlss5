"""Compile regression cases against the actual backported predicates.

Run from an x64 Visual Studio developer prompt: python tools/verify_helldivers2.py
This exercises path classification and all four DXGI overlay gates without a GPU.
It does not test game rendering, Streamline initialization, or DLSSNR evaluation.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def block(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def generate():
    util = (ROOT / "OptiScaler/Util.cpp").read_text(encoding="utf-8-sig")
    sys = (ROOT / "OptiScaler/SysUtils.h").read_text(encoding="utf-8-sig")
    dxgi = (ROOT / "OptiScaler/hooks/DxgiFactory_Hooks.cpp").read_text(encoding="utf-8-sig")
    # Composition swapchains require explicit nonzero dimensions; only the
    # window-backed creation APIs accept zero as "use the window size".
    windows = "\n".join(block(dxgi, f"HRESULT DxgiFactoryHooks::{name}(") for name in (
        "CreateSwapChain", "CreateSwapChainForHwnd", "DLSSGCreateSwapChain", "DLSSGCreateSwapChainForHwnd"))
    predicates = re.findall(r'if\s*\(([^{};]+)\)\s*\{\s*LOG_WARN\("Overlay call!"\)', windows)
    if len(predicates) != 4:
        raise RuntimeError(f"Expected four overlay gates, found {len(predicates)}")
    wrappers = []
    for index, expression in enumerate(predicates):
        wrappers.append(f"bool overlay{index}(unsigned width, unsigned height) {{\n"
                        "struct Desc { struct Size { unsigned Width, Height; } BufferDesc; "
                        "unsigned Width, Height; } desc {{width, height}, width, height};\n"
                        f"auto pDesc = &desc; return {expression}; }}")
    return r'''
#include <algorithm>
#include <cassert>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <string>
namespace Util { bool IsSubpath(const std::filesystem::path&, const std::filesystem::path&); }
''' + block(sys, "inline static void to_lower_in_place(std::wstring&") + "\n" + block(
        util, "bool Util::IsSubpath(") + "\n" + "\n".join(wrappers) + r'''
int main() {
    const auto base = LR"(C:\Game\OptiScaler\streamline)";
    assert(Util::IsSubpath(LR"(c:\game\OPTISCALER\STREAMLINE\sl.interposer.dll)", base));
    assert(Util::IsSubpath(LR"(C:\Game\OptiScaler\streamline\development\sl.common.dll)", base));
    assert(!Util::IsSubpath(LR"(C:\Game\sl.interposer.dll)", base));
    assert(!Util::IsSubpath(LR"(C:\Game\OptiScaler\streamline-other\sl.interposer.dll)", base));
    assert(!Util::IsSubpath(LR"(D:\Game\OptiScaler\streamline\sl.interposer.dll)", base));
    assert(!Util::IsSubpath(base, base));
    assert(!Util::IsSubpath(LR"(C:\Game\OptiScaler\streamline\..\sl.interposer.dll)", base));
    for (auto gate : {overlay0, overlay1, overlay2, overlay3}) {
        assert(!gate(0, 0));
        assert(!gate(0, 1080));
        assert(!gate(1920, 0));
        assert(!gate(1920, 1080));
        assert(!gate(100, 100));
        assert(gate(64, 64));
        assert(gate(99, 1080));
        assert(gate(1920, 99));
        assert(gate(0, 64));
    }
    std::cout << "PASS: 7 Windows path cases and 36 DXGI overlay cases\n";
}
'''


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="hd2-regression-") as temp:
        folder = Path(temp)
        (folder / "regression.cpp").write_text(generate(), encoding="utf-8")
        subprocess.run(["cl", "/nologo", "/EHsc", "/std:c++17", "regression.cpp"], cwd=temp, check=True)
        subprocess.run([str(folder / "regression.exe")], check=True)
