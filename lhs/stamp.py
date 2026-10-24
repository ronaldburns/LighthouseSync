"""Stamps a LighthouseSync build: writes src/common/lhs_build.h (version, repository, commit) for the driver and the
dashboard. Without it a build is a local one, "<upstream release>-dev".

  python lhs/stamp.py v1.26.3 [owner/name]
  python lhs/stamp.py --clear           back to a local build

The repository defaults to GITHUB_REPOSITORY (set in GitHub Actions), else origin's. It is where the dashboard looks
for updates, so a fork's releases update from the fork. The headset builds (magisk/build_module.py, frame/build.py)
take the same version from the LHS_VERSION environment variable.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "src", "common", "lhs_build.h")


def git(*args):
    try:
        return subprocess.run(["git", "-C", ROOT, *args], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def origin_repo():
    m = re.search(r"github\.com[:/]([^/]+/[^/]+?)(\.git)?$", git("remote", "get-url", "origin"))
    return m.group(1) if m else "ronaldburns/LighthouseSync"


def main():
    if sys.argv[1:] == ["--clear"]:
        if os.path.exists(OUT):
            os.remove(OUT)
        print(f"removed {OUT}")
        return
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    tag = sys.argv[1]
    m = re.fullmatch(r"v?(\d+)\.(\d+)(?:\.(\d+))?", tag)
    if not m:
        sys.exit(f"{tag}: expected a version like v1.26.3")
    version = tag.lstrip("v")
    nums = [int(m.group(1)), int(m.group(2)), int(m.group(3) or 0), 0]
    repo = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("GITHUB_REPOSITORY") or origin_repo()
    commit = (os.environ.get("GITHUB_SHA") or git("rev-parse", "HEAD"))[:12]
    text = (
        "// written by lhs/stamp.py: don't edit, don't commit\n"
        "#pragma once\n"
        f'#define LHS_VERSION "{version}"\n'
        f"#define LHS_VERSION_NUM {','.join(map(str, nums))}\n"
        f'#define LHS_REPO "{repo}"\n'
        f'#define LHS_COMMIT "{commit}"\n'
    )
    with open(OUT, "w", newline="\n") as f:
        f.write(text)
    print(f"stamped {version} from {repo} at {commit or 'no commit'}")


if __name__ == "__main__":
    main()
