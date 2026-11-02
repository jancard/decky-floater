#!/usr/bin/env python3
"""Create a Decky-installable ZIP with the same layout the Decky CLI and Plugin Store produce."""

import hashlib
import json
from pathlib import Path
import sys
import zipfile

ROOT = Path(__file__).resolve().parent.parent
ROOT_FILES = ("LICENSE", "README.md", "main.py", "package.json", "plugin.json")


def collect(helper):
    files = {name: ROOT / name for name in ROOT_FILES}
    files["dist/index.js"] = ROOT / "dist" / "index.js"
    files["bin/pip-overlay"] = helper
    # The Decky CLI copies defaults/ into the plugin root.
    for path in sorted((ROOT / "defaults").rglob("*")):
        if path.is_file():
            files[path.relative_to(ROOT / "defaults").as_posix()] = path
    return files


def main():
    helper = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "backend" / "out" / "pip-overlay"
    files = collect(helper)
    missing = [str(path) for path in files.values() if not path.is_file()]
    if missing:
        raise SystemExit(f"Missing {', '.join(missing)}; run npm run build and make -C backend first")

    version = json.loads((ROOT / "package.json").read_text())["version"]
    output = ROOT / "release" / f"floater-v{version}.zip"
    output.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, path in files.items():
            info = zipfile.ZipInfo.from_file(path, f"Floater/{name}")
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o755 if name.startswith("bin/") else 0o644) << 16
            archive.writestr(info, path.read_bytes())
    with zipfile.ZipFile(output) as archive:
        assert archive.testzip() is None
        assert len(archive.namelist()) == len(files)

    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_name(output.name + ".sha256").write_text(f"{digest}  {output.name}\n")
    print(output)


if __name__ == "__main__":
    main()
