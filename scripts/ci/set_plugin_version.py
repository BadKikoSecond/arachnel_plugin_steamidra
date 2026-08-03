#!/usr/bin/env python3
"""Set manifest and C++ plugin version from a release tag."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: set_plugin_version.py <tag>", file=sys.stderr)
        return 2
    version = re.sub(r"^v", "", sys.argv[1].strip(), count=1)
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[\w.]+)?", version):
        print(f"invalid semver tag: {sys.argv[1]!r}", file=sys.stderr)
        return 2

    root = Path(__file__).resolve().parents[2]
    manifest_path = root / "plugin.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["version"] = version
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    cpp_path = root / "src" / "steamidra_plugin.cpp"
    cpp = cpp_path.read_text(encoding="utf-8")
    # version() reads plugin.json first; keep the fallback QStringLiteral in sync.
    updated, count = re.subn(
        r'(QString SteamidraPlugin::version\(\) const\s*\{.*?)(return QStringLiteral\(")[^"]+("\);)(\s*\})',
        rf"\g<1>\g<2>{version}\g<3>\g<4>",
        cpp,
        count=1,
        flags=re.DOTALL,
    )
    if count != 1:
        print("could not update C++ version", file=sys.stderr)
        return 1
    cpp_path.write_text(updated, encoding="utf-8")
    print(f"plugin version -> {version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
