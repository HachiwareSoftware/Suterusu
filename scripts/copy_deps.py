#!/usr/bin/env python3
import os
import platform
import shutil
from pathlib import Path

# Kind of hardcoded here but okay. (not okay though)
msys2_path = Path(os.getenv("MSYS2_ROOT", "C:/msys64/ucrt64/bin/"))
msys2_msys_path = Path(os.getenv("MSYS2_MSYS_ROOT", "C:/msys64/usr/bin/"))
# Scoop installation fallback
if not (msys2_path.exists() and msys2_msys_path.exists()):
    userprofile = Path(os.getenv("USERPROFILE") or Path.home())
    msys2_path = userprofile / "scoop" / "apps" / "msys2" / "current" / "ucrt64" / "bin"
    msys2_msys_path = userprofile / "scoop" / "apps" / "msys2" / "current" / "usr" / "bin"
source_path = Path(os.getenv("MESON_SOURCE_ROOT"))
build_path = Path(os.getenv("MESON_BUILD_ROOT"))
install_path = build_path.parent / "bin"
install_path.mkdir(exist_ok=True)
# Copy all dlls to the bin directory
for dll in build_path.glob("**/*.dll"):
    print(f"Copying {dll} to {install_path / dll.name}")
    shutil.copy(dll, install_path / dll.name)
# Untested but okay.
for so in build_path.glob("**/*.so"):
    print(f"Copying {so} to {install_path / so.name}")
    shutil.copy(so, install_path / so.name)
for dylib in build_path.glob("**/*.dylib"):
    print(f"Copying {dylib} to {install_path / dylib.name}")
    shutil.copy(dylib, install_path / dylib.name)

if platform.system() == "Windows":
    # Fuck Windows.
    if "msvc" not in str(build_path):
        print("Begin copying MSYS2 dependencies...")
        for dll_line in Path("../scripts/dlls_list.txt").read_text().splitlines():
            try:
                first_str = dll_line.strip().split()[0]
            except IndexError:
                continue
            if Path(first_str).suffix != ".dll":
                continue
            full_dll_path = Path(msys2_path) / first_str
            if not full_dll_path.exists():
                full_dll_path = Path(msys2_msys_path) / first_str
            if not full_dll_path.exists():
                continue
            print(f"Copying {full_dll_path} to {install_path / first_str}")
            shutil.copy(full_dll_path, install_path / first_str)
