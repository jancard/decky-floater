#!/usr/bin/env python3
"""Check the newest Gamescope and MangoHud tags for the private interfaces Floater relies on.

Exits non-zero when a tag newer than the one verified in compat.json appears (test it on a
device, then update "verified") or when a required marker is missing (that release likely breaks
Floater).
"""

import io
import json
import re
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

CONFIG = Path(__file__).with_name("compat.json")
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp")
VERSION_TAG = re.compile(r"^v?(\d+(?:\.\d+)*)$")


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "floater-compat-check"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def latest_tag(repo):
    # Gamescope publishes tags but not GitHub releases.
    refs = subprocess.run(["git", "ls-remote", "--tags", "--refs", f"https://github.com/{repo}.git"],
                          check=True, capture_output=True, text=True).stdout
    versions = []
    for line in refs.splitlines():
        tag = line.rsplit("refs/tags/", 1)[-1]
        match = VERSION_TAG.match(tag)
        if match:
            versions.append((tuple(int(part) for part in match.group(1).split(".")), tag))
    return max(versions)[1]


def source_text(repo, tag):
    archive = fetch(f"https://codeload.github.com/{repo}/tar.gz/refs/tags/{tag}")
    texts = []
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as tar:
        for member in tar:
            if member.isfile() and "/src/" in member.name and member.name.endswith(SOURCE_SUFFIXES):
                texts.append(tar.extractfile(member).read().decode("utf-8", "replace"))
    return "\n".join(texts)


def main():
    config = json.loads(CONFIG.read_text())
    problems = 0
    for name, upstream in config.items():
        tag = latest_tag(upstream["repo"])
        text = source_text(upstream["repo"], tag)
        print(f"{name}: latest tag {tag}, verified {upstream['verified']}")
        for label, pattern in upstream["markers"].items():
            found = re.search(pattern, text) is not None
            print(f"  {'ok' if found else 'MISSING'}: {label}")
            if not found:
                print(f"::error::{name} {tag}: {label} not found; Floater likely needs changes")
                problems += 1
        if tag != upstream["verified"]:
            print(f"::warning::{name} {tag} is newer than the verified {upstream['verified']}; "
                  f"test Floater with it, then update {CONFIG.name}")
            problems += 1
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
