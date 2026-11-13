#!/usr/bin/env python3
"""Turn a Cargo.lock into flatpak-builder sources, so cargo builds offline from vendored crates.

Usage: gen-cargo-sources.py Cargo.lock > pngquant-cargo-sources.json
Only crates.io packages are supported (pngquant has no git dependencies).
"""
import json
import sys
import tomllib

CONFIG = ('[source.vendored-sources]\ndirectory = "cargo/vendor"\n\n'
          '[source.crates-io]\nreplace-with = "vendored-sources"\n')

with open(sys.argv[1], "rb") as f:
    lock = tomllib.load(f)

sources = []
for pkg in lock["package"]:
    src = pkg.get("source", "")
    if not src:
        continue  # the root crate itself
    if not src.startswith("registry+"):
        sys.exit(f"unsupported source for {pkg['name']}: {src}")
    name, ver, sha = pkg["name"], pkg["version"], pkg["checksum"]
    dest = f"cargo/vendor/{name}-{ver}"
    sources.append({"type": "archive", "archive-type": "tar-gzip",
                    "url": f"https://static.crates.io/crates/{name}/{name}-{ver}.crate",
                    "sha256": sha, "dest": dest})
    sources.append({"type": "inline", "contents": json.dumps({"package": sha, "files": {}}),
                    "dest": dest, "dest-filename": ".cargo-checksum.json"})
sources.append({"type": "inline", "contents": CONFIG, "dest": "cargo", "dest-filename": "config.toml"})
json.dump(sources, sys.stdout, indent=2)
print()
