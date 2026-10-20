"""Create GitHub source/release archives from an explicit file allowlist."""
import argparse
import hashlib
from pathlib import Path
import re
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIRS = ("src", "include", "cmake", "config", "tests", "tools", "docs", "licenses", ".github")
SOURCE_FILES = ("README.md", "THIRD_PARTY_NOTICES.md", "CHANGELOG.md",
                ".gitignore", ".gitattributes", "CMakeLists.txt", "CMakePresets.json",
                "deps.lock", "build.bat", "resource.h", "SanFrameOptimizer.def")
SUFFIXES = {".cpp", ".hpp", ".h", ".hlsl", ".cmake", ".py", ".md", ".txt", ".ini", ".yml", ".yaml"}


def source_files():
    files = [ROOT / name for name in SOURCE_FILES]
    for name in SOURCE_DIRS:
        files.extend(p for p in (ROOT / name).rglob("*") if p.is_file() and
                     p.suffix in SUFFIXES and "__pycache__" not in p.parts)
    files.extend((ROOT / "resources").glob("*.hlsl"))
    return sorted(set(files))


def write_zip(path, files, prefix=""):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for source, relative in files:
            archive.write(source, prefix + relative)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--configuration", default="Release", choices=("Release", "Debug"))
    parser.add_argument("--source-only", action="store_true",
                        help="Package the source without requiring a local ASI build")
    args = parser.parse_args()
    version = re.search(r"project\(SanFrameOptimizer VERSION ([\d.]+)",
                        (ROOT / "CMakeLists.txt").read_text()).group(1)
    output = ROOT / "dist"
    output.mkdir(exist_ok=True)
    files = source_files()
    missing = [str(p) for p in files if not p.is_file()]
    if missing:
        parser.error("Missing source files: " + ", ".join(missing))
    build = args.build_dir.resolve()
    binaries = [build / args.configuration / name for name in
                ("SanFrameOptimizer.asi", "SanFrameOptimizer.ini")]
    if not args.source_only and not all(p.is_file() for p in binaries):
        parser.error("Build ASI/INI before packaging")
    source_zip = output / f"SanFrameOptimizer-{version}-source.zip"
    release_zip = output / f"SanFrameOptimizer-{version}-win32.zip"
    write_zip(source_zip, [(p, p.relative_to(ROOT).as_posix()) for p in files],
              f"SanFrameOptimizer-{version}/")
    archives = [source_zip]
    if not args.source_only:
        release = [(p, p.name) for p in binaries]
        release.extend((ROOT / name, name) for name in
                       ("README.md", "THIRD_PARTY_NOTICES.md", "CHANGELOG.md", "deps.lock"))
        release.extend((p, p.relative_to(ROOT).as_posix()) for p in sorted((ROOT / "licenses").glob("*.txt")))
        release.extend((p, p.relative_to(ROOT).as_posix()) for p in sorted((ROOT / "docs").rglob("*.md")))
        write_zip(release_zip, release)
        archives.append(release_zip)
    checksums = "".join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n"
                        for p in archives)
    (output / "SHA256SUMS.txt").write_text(checksums, encoding="utf-8")
    for path in archives:
        print(f"{path}: {path.stat().st_size:,} bytes")


if __name__ == "__main__":
    main()
