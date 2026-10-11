"""Format maintained port sources, or check them without changing files."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check", action="store_true", help="Fail if formatting changes are needed"
    )
    args = parser.parse_args()

    local = ROOT / ".format-tools"
    environment = dict(os.environ)
    if local.is_dir():
        environment["PYTHONPATH"] = str(local) + os.pathsep + environment.get("PYTHONPATH", "")
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    environment["BLACK_CACHE_DIR"] = str(local / "black-cache")

    bundled = (
        local
        / "clang_format"
        / "data"
        / "bin"
        / ("clang-format.exe" if os.name == "nt" else "clang-format")
    )
    formatter = str(bundled) if bundled.is_file() else shutil.which("clang-format")
    if not formatter:
        parser.error("Install tools/format-requirements.txt before running this command")

    c_sources = sorted(
        path
        for folder in ("main", "tests")
        for path in (ROOT / folder).iterdir()
        if path.suffix in (".c", ".h")
    )
    python_sources = sorted(
        path for folder in ("tests", "tools") for path in (ROOT / folder).glob("*.py")
    )
    c_args = ["--dry-run", "--Werror"] if args.check else ["-i"]
    c_result = subprocess.run([formatter, *c_args, *map(str, c_sources)], cwd=ROOT, env=environment)
    python_args = ["--check"] if args.check else []
    python_result = subprocess.run(
        [sys.executable, "-m", "black", *python_args, *map(str, python_sources)],
        cwd=ROOT,
        env=environment,
    )
    if c_result.returncode or python_result.returncode:
        return 1
    print(
        f"Formatting {'check passed' if args.check else 'applied'}: "
        f"{len(c_sources)} C/header and {len(python_sources)} Python files"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
