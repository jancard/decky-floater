#!/usr/bin/env python3
"""Usage: forgejo-release.py TAG FILE...

Create (or reuse) a draft Forgejo release for TAG and attach FILEs. Uses the Forgejo Actions
environment: GITHUB_SERVER_URL, GITHUB_REPOSITORY, GITHUB_SHA and RELEASE_TOKEN.
"""

import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path


def api(method, path, body=None, content_type="application/json"):
    url = f"{os.environ['GITHUB_SERVER_URL']}/api/v1/repos/{os.environ['GITHUB_REPOSITORY']}{path}"
    request = urllib.request.Request(url, data=body, method=method, headers={
        "Authorization": f"token {os.environ['RELEASE_TOKEN']}",
        "Accept": "application/json",
        "Content-Type": content_type,
    })
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.loads(response.read() or b"null")


def find_or_create(tag):
    try:
        return api("GET", f"/releases/tags/{urllib.parse.quote(tag)}")
    except urllib.error.HTTPError as error:
        if error.code != 404:
            raise
    notes = (f"Built by CI from {os.environ.get('GITHUB_SHA', 'unknown')[:12]}.\n\n"
             "Test on a Steam Deck and a desktop Game Mode setup before publishing this draft.")
    body = {"tag_name": tag, "name": f"Floater {tag}", "body": notes,
            "draft": True, "prerelease": "-" in tag}
    return api("POST", "/releases", json.dumps(body).encode())


def upload(release, path):
    if any(asset["name"] == path.name for asset in release.get("assets") or []):
        print(f"{path.name} already attached")
        return
    boundary = uuid.uuid4().hex
    body = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"attachment\"; "
            f"filename=\"{path.name}\"\r\nContent-Type: application/octet-stream\r\n\r\n").encode()
    body += path.read_bytes() + f"\r\n--{boundary}--\r\n".encode()
    api("POST", f"/releases/{release['id']}/assets?name={urllib.parse.quote(path.name)}", body,
        f"multipart/form-data; boundary={boundary}")
    print(f"attached {path.name}")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__.splitlines()[0])
    release = find_or_create(sys.argv[1])
    for name in sys.argv[2:]:
        upload(release, Path(name))
    print(release.get("html_url", ""))


if __name__ == "__main__":
    main()
