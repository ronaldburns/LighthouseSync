"""Packs a LighthouseSync release into out/lhs-release/ from what build.bat, magisk/build_module.py, frame/build.py and
lhs/build_panel.py built (LHS_VERSION and LHS_REPO set for all of them, as the release workflow does):

  LighthouseSync-steamvr-<version>.zip         the SteamVR driver folder questlhsync/ (driver, dashboard, openvr_api.dll)
  LighthouseSync-quest-module-<version>.zip    the Quest's Magisk module
  LighthouseSync-frame-<version>.tar.gz        the Steam Frame's package (QuestLHSync-frame/ with its install.sh),
                                               and the headset panel in QuestLHSync-frame/lhs/
  install.ps1, install-frame.sh                the one-line installers, pointed at the repository
  quest-update.json                            Magisk's update feed for the module
  SHA256SUMS                                   all of the above

  python lhs/package.py <version> <owner/name>

Each package carries LICENSE and the third-party notices, as upstream's release.py does. Refuses when a build is
missing or older than its sources, so a release never ships stale binaries.
"""
import hashlib
import io
import json
import os
import shutil
import sys
import tarfile
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVER = os.path.join(ROOT, "driver", "questlhsync")
BIN = os.path.join(DRIVER, "bin", "win64")
OUT = os.path.join(ROOT, "out")
DEST = os.path.join(OUT, "lhs-release")
PANEL = os.path.join(ROOT, "build", "panel", "lighthousesync-panel")
NOTICES = (("LICENSE", "LICENSE"), ("THIRD_PARTY_NOTICES.md", "THIRD_PARTY_NOTICES.md"),
           (os.path.join("lhs", "THIRD_PARTY_NOTICES.md"), "THIRD_PARTY_NOTICES-LighthouseSync.md"))


def newest(*dirs):
    t = 0
    for d in dirs:
        for root, _, files in os.walk(os.path.join(ROOT, d)):
            t = max([t] + [os.path.getmtime(os.path.join(root, f)) for f in files])
    return t


def notices():
    for src, name in NOTICES:
        with open(os.path.join(ROOT, src), "rb") as f:
            yield name, f.read()


def steamvr_zip(out):
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for rel in ("driver.vrdrivermanifest", "resources/settings/default.vrsettings",
                    "bin/win64/driver_questlhsync.dll", "bin/win64/QuestLHSync.exe", "bin/win64/openvr_api.dll"):
            z.write(os.path.join(DRIVER, rel), "questlhsync/" + rel)
        for name, data in notices():
            z.writestr(name, data)


def module_zip(src_path, out):
    with zipfile.ZipFile(src_path) as src, zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for info in src.infolist():  # as built: Magisk reads its META-INF and module.prop from it
            z.writestr(info, src.read(info))
        for name, data in notices():
            z.writestr(name, data)


def frame_tar(src_path, out):
    panel = [("lhs/lighthousesync-panel", PANEL, 0o755)]
    for name in ("lighthousesync-panel.service", "lighthousesync.desktop"):
        panel.append((f"lhs/{name}", os.path.join(ROOT, "lhs", "panel", name), 0o644))
    for n in (48, 128, 256):
        panel.append((f"lhs/icons/lighthousesync-{n}.png", os.path.join(ROOT, "lhs", "panel", "icons", f"lighthousesync-{n}.png"), 0o644))
    with tarfile.open(src_path, "r:gz") as src, tarfile.open(out, "w:gz") as t:
        for m in src.getmembers():
            t.addfile(m, src.extractfile(m) if m.isfile() else None)

        def add(name, data, mode):
            info = tarfile.TarInfo(f"QuestLHSync-frame/{name}")
            info.size, info.mode, info.mtime = len(data), mode, int(time.time())
            t.addfile(info, io.BytesIO(data))
        for name, path, mode in panel:  # install-frame.sh installs these (upstream's install.sh doesn't know them)
            with open(path, "rb") as f:
                data = f.read()
            add(name, data if name.endswith((".png", "-panel")) else data.replace(b"\r\n", b"\n"), mode)
        for name, data in notices():
            add(name, data, 0o644)


def script(name, repo, out):
    with open(os.path.join(ROOT, "lhs", name), encoding="utf-8") as f:
        text = f.read().replace("\r\n", "\n")
    if "@REPO@" not in text:
        sys.exit(f"lhs/{name} has no @REPO@ to fill in")
    with open(out, "w", encoding="utf-8", newline="\r\n" if name.endswith(".ps1") else "\n") as f:
        f.write(text.replace("@REPO@", repo))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    ver, repo = sys.argv[1].lstrip("v"), sys.argv[2]
    tag = "v" + ver
    module = os.path.join(OUT, f"QuestLHSync-quest-module-{ver}.zip")
    frame = os.path.join(OUT, f"QuestLHSync-frame-{tag}.tar.gz")
    checks = [
        (os.path.join(BIN, "driver_questlhsync.dll"), newest("src/driver", "src/common"), "build.bat"),
        (os.path.join(BIN, "QuestLHSync.exe"), newest("src/dashboard", "src/common", "third_party/imgui"), "build.bat"),
        (module, newest("magisk/src", "magisk/module", "src/headset"), "python magisk\\build_module.py"),
        (frame, newest("frame/src", "frame/driver", "frame/package", "src/headset"), "python frame\\build.py"),
        (PANEL, newest("src/panel", "src/dashboard", "src/common", "third_party/imgui", "third_party/vulkan"),
         "python lhs\\build_panel.py"),
    ]
    for path, src, how in checks:
        if not os.path.exists(path) or os.path.getmtime(path) < src:
            sys.exit(f"{os.path.relpath(path, ROOT)} is missing or older than its sources: run {how} "
                     f"with LHS_VERSION={ver}")
    with zipfile.ZipFile(module) as z:
        prop = z.read("module.prop").decode()
    code = next((int(line.split("=", 1)[1]) for line in prop.splitlines() if line.startswith("versionCode=")), 0)
    if f"version={tag}" not in prop or "updateJson=" not in prop:
        sys.exit(f"{os.path.relpath(module, ROOT)} wasn't built for {tag} from {repo}: set LHS_VERSION and LHS_REPO")

    shutil.rmtree(DEST, ignore_errors=True)
    os.makedirs(DEST)
    names = {
        "steamvr": f"LighthouseSync-steamvr-{ver}.zip",
        "quest": f"LighthouseSync-quest-module-{ver}.zip",
        "frame": f"LighthouseSync-frame-{ver}.tar.gz",
    }
    steamvr_zip(os.path.join(DEST, names["steamvr"]))
    module_zip(module, os.path.join(DEST, names["quest"]))
    frame_tar(frame, os.path.join(DEST, names["frame"]))
    script("install.ps1", repo, os.path.join(DEST, "install.ps1"))
    script("install-frame.sh", repo, os.path.join(DEST, "install-frame.sh"))
    with open(os.path.join(DEST, "quest-update.json"), "w", newline="\n") as f:
        json.dump({"version": tag, "versionCode": code,
                   "zipUrl": f"https://github.com/{repo}/releases/download/{tag}/{names['quest']}",
                   "changelog": f"https://github.com/{repo}/releases/download/{tag}/changelog.md"}, f, indent=2)
        f.write("\n")
    with open(os.path.join(DEST, "changelog.md"), "w", newline="\n") as f:  # what Magisk shows; the workflow fills it in
        f.write(f"## LighthouseSync {tag}\n\nSee https://github.com/{repo}/releases/tag/{tag}\n")

    lines = []
    for name in sorted(os.listdir(DEST)):
        with open(os.path.join(DEST, name), "rb") as f:
            lines.append(f"{hashlib.sha256(f.read()).hexdigest()}  {name}\n")
    with open(os.path.join(DEST, "SHA256SUMS"), "w", newline="\n") as f:
        f.writelines(lines)
    for name in sorted(os.listdir(DEST)):
        print(f"  {name}  ({os.path.getsize(os.path.join(DEST, name)) / 1e6:.2f} MB)")
    print(f"packed {tag} from {repo} into {DEST}")


if __name__ == "__main__":
    main()
