#!/usr/bin/env python3
"""Create a Decky-installable ZIP containing only the plugin runtime files."""

import json
from pathlib import Path
import sys
import zipfile


def main():
    root = Path(__file__).resolve().parent
    metadata = json.loads((root / "package.json").read_text())
    helper = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "backend" / "out" / "pip-overlay"
    files = {
        name: root / name for name in (
            "LICENSE", "THIRD_PARTY_NOTICES.md", "main.py", "package.json", "plugin.json",
            "dist/index.js",
        )
    }
    files["bin/pip-overlay"] = helper
    for name, path in files.items():
        if not path.is_file():
            raise SystemExit(f"Missing {path}; run npm run build and make -C backend first")
    output = root / "release" / f"floater-{metadata['version']}.zip"
    output.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, path in files.items():
            info = zipfile.ZipInfo.from_file(path, f"floater/{name}")
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o755 if name.startswith("bin/") else 0o644) << 16
            archive.writestr(info, path.read_bytes())
    with zipfile.ZipFile(output) as archive:
        assert archive.testzip() is None
        assert len(archive.namelist()) == len(files)
    print(output)


if __name__ == "__main__":
    main()
