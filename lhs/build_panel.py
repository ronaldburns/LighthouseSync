"""Builds the Steam Frame's panel: build/panel/lighthousesync-panel (src/panel, with the dashboard's pages code from
src/dashboard and Dear ImGui). lhs/package.py adds it to the Frame package.

Compilers as frame/build.py finds them: cc/c++ on an arm64 Linux, else Zig (PATH, or the ZIG environment variable)
cross-compiling against the same glibc. lhs/stamp.py's src/common/lhs_build.h gives it the release's version.

  python lhs/build_panel.py
"""
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "frame"))
from build import compilers  # noqa: E402

BUILD = os.path.join(ROOT, "build", "panel")
OUT = os.path.join(BUILD, "lighthousesync-panel")
SOURCES = [os.path.join("src", "panel", f) for f in ("panel_main.cpp", "panel_ui.cpp", "vk.cpp", "openvr_shim.cpp")] + \
          [os.path.join("src", "dashboard", f) for f in ("theme.cpp", "swr.cpp", "icon.cpp")] + \
          [os.path.join("third_party", "imgui", f) for f in ("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp",
                                                              "imgui_widgets.cpp")]
INCLUDES = ["src/common", "src/dashboard", "third_party/imgui", "third_party/openvr/headers", "third_party/vulkan/include"]
FLAGS = ["-std=c++20", "-O2", "-Wall", "-DNDEBUG", "-DIMGUI_DISABLE_DEMO_WINDOWS", "-DIMGUI_DISABLE_DEBUG_TOOLS",
         "-ffunction-sections", "-fdata-sections"]


def main():
    _, cxx = compilers()
    os.makedirs(BUILD, exist_ok=True)
    inc = [f"-I{os.path.join(ROOT, i)}" for i in INCLUDES]

    def compile_one(src):
        obj = os.path.join(BUILD, os.path.splitext(os.path.basename(src))[0] + ".o")
        r = subprocess.run([*cxx, *FLAGS, *inc, "-c", os.path.join(ROOT, src), "-o", obj], capture_output=True, text=True)
        return src, obj, r

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        results = list(pool.map(compile_one, SOURCES))
    failed = False
    for src, _, r in results:
        if r.stdout or r.stderr:
            print(f"{src}:\n{r.stdout}{r.stderr}", end="")
        failed |= r.returncode != 0
    if failed:
        sys.exit("the panel didn't compile")
    subprocess.run([*cxx, *[obj for _, obj, _ in results], "-Wl,--gc-sections", "-ldl", "-pthread", "-s", "-o", OUT],
                   check=True)
    print(f"built {OUT} ({os.path.getsize(OUT) / 1e3:.0f} KB)")
    return OUT


if __name__ == "__main__":
    main()
