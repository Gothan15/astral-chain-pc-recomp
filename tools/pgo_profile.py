"""Merge the .profraw files of an instrumented game into tools/data/pgo/<version>.proftext.xz, the profile the
installer optimises the generated code with (build_game.py --release-opt).

    .venv/bin/python tools/build_game.py --version v101 --out build/pgo-gen --pgo-generate build/pgo-raw
    (run build/pgo-gen/astralchain, copied into build/v101, on the scenes to cover; each run adds to build/pgo-raw)
    .venv/bin/python tools/pgo_profile.py --version v101 build/pgo-raw

The profile is sparse: functions no run executed are left out, so clang compiles them as it does without a profile
instead of treating them as cold. It is text compressed with xz: function names (f_<guest address>), one hash of each
function's control flow and its block counts, nothing else from the game. A function whose generated C changes
since the profile was taken loses its profile (clang warns and compiles it without), so take a new one when the
generator's output changes.

Needs llvm-profdata of the clang major version the release ships (tools/make_release_linux.py LLVM_VERSION).
Standard library only.
"""
import argparse
import lzma
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PGO_DIR = ROOT / 'tools' / 'data' / 'pgo'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('raw', nargs='+', type=Path, help='.profraw files or directories holding them')
    ap.add_argument('--version', default='v101', help='game version (default v101)')
    ap.add_argument('--llvm-profdata', default=shutil.which('llvm-profdata') or 'llvm-profdata')
    args = ap.parse_args()
    files = [f for p in args.raw for f in (sorted(p.glob('*.profraw')) if p.is_dir() else [p])]
    if not files:
        sys.exit('no .profraw files')
    out = PGO_DIR / f'{args.version}.proftext.xz'
    with tempfile.TemporaryDirectory(dir=ROOT / 'build') as tmp:
        text = Path(tmp) / 'profile.proftext'
        subprocess.run([args.llvm_profdata, 'merge', '--sparse', '--text', '-o', text, *files], check=True)
        data = text.read_bytes()
    funcs = len(re.findall(rb'^# Func Hash:', data, re.MULTILINE))
    PGO_DIR.mkdir(parents=True, exist_ok=True)
    out.write_bytes(lzma.compress(data, preset=9 | lzma.PRESET_EXTREME))
    print(f'{out.relative_to(ROOT).as_posix()}: {len(files)} raw files, {funcs} functions with counts, '
          f'{len(data) / 1e6:.1f} MB of text, {out.stat().st_size / 1e3:.0f} kB compressed')


if __name__ == '__main__':
    main()
