#!/usr/bin/env python3
"""Reject Windows binaries that need a DLL the release does not ship.

There are two Windows build trees and they are not interchangeable: the static one links
everything into the executable, the dynamic one leaves it importing SDL2, glew, spdlog, fmt,
tinyxml2, libzip, zlib and bzip2 as loose DLLs. v1.0.0 was packaged from the wrong one and
only caught by hand. Reading the import table settles which tree an artifact came from.

    python tools/check_windows_deps.py dist/G-Diffuser.exe dist/gdx-extract.exe

Exits non-zero if any binary imports something outside the set Windows itself provides.
"""

import os
import struct
import sys

# DLLs present on a stock Windows 10/11 install. Anything else has to travel with the game,
# and the release ships no DLLs at all, so anything else is a packaging mistake.
SYSTEM_DLLS = {
    "advapi32.dll", "avrt.dll", "bcrypt.dll", "cfgmgr32.dll", "comctl32.dll", "comdlg32.dll",
    "crypt32.dll", "d3d11.dll", "d3d12.dll", "d3dcompiler_47.dll", "dbghelp.dll", "dinput8.dll",
    "dsound.dll", "dwmapi.dll", "dxgi.dll", "gdi32.dll", "gdi32full.dll", "hid.dll",
    "imm32.dll", "iphlpapi.dll", "kernel32.dll", "kernelbase.dll", "mf.dll", "mfplat.dll",
    "mfreadwrite.dll", "msvcrt.dll", "netapi32.dll", "normaliz.dll", "ntdll.dll", "ole32.dll",
    "oleacc.dll", "oleaut32.dll", "opengl32.dll", "powrprof.dll", "propsys.dll", "psapi.dll",
    "rpcrt4.dll", "secur32.dll", "setupapi.dll", "shcore.dll", "shell32.dll", "shlwapi.dll",
    "ucrtbase.dll", "user32.dll", "userenv.dll", "uxtheme.dll", "version.dll", "winhttp.dll",
    "winmm.dll", "wintrust.dll", "wldap32.dll", "ws2_32.dll", "xinput1_4.dll",
}

# The API-set stubs the loader resolves internally; they are not real files to ship.
SYSTEM_PREFIXES = ("api-ms-win-", "ext-ms-win-")


def read_imports(path):
    """DLL names from a PE import directory."""
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:2] != b"MZ":
        raise ValueError(f"{path}: not a PE image")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path}: PE signature missing")

    section_count, _, _, _, optional_size, _ = struct.unpack_from("<HIIIHH", data, pe + 6)
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    if magic == 0x20B:        # PE32+
        directories = optional + 112
    elif magic == 0x10B:      # PE32
        directories = optional + 96
    else:
        raise ValueError(f"{path}: unknown optional header magic {magic:#x}")

    # Data directory 1 is the import table.
    import_rva = struct.unpack_from("<I", data, directories + 8)[0]
    if import_rva == 0:
        return []

    sections = []
    for index in range(section_count):
        base = optional + optional_size + index * 40
        virtual_size, virtual_addr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, base + 8)
        sections.append((virtual_addr, max(virtual_size, raw_size), raw_ptr))

    def to_offset(rva):
        for virtual_addr, span, raw_ptr in sections:
            if virtual_addr <= rva < virtual_addr + span:
                return raw_ptr + (rva - virtual_addr)
        raise ValueError(f"{path}: RVA {rva:#x} is outside every section")

    names, cursor = [], to_offset(import_rva)
    while True:
        entry = data[cursor:cursor + 20]
        if len(entry) < 20 or entry == b"\0" * 20:
            break
        name_rva = struct.unpack_from("<I", entry, 12)[0]
        if name_rva == 0:
            break
        start = to_offset(name_rva)
        names.append(data[start:data.index(b"\0", start)].decode("ascii", "replace"))
        cursor += 20
    return names


def check(path):
    name = os.path.basename(path)
    imports = read_imports(path)
    extra = [dll for dll in imports
             if dll.lower() not in SYSTEM_DLLS
             and not dll.lower().startswith(SYSTEM_PREFIXES)]
    if extra:
        print(f"FAIL  {name}: needs DLLs the release does not ship: " + ", ".join(sorted(extra)))
        print(f"      This is the dynamic build tree. Releases come from the static one:")
        print(f"      cmake -S . -B build/x64 -DUSE_AUTO_VCPKG=ON")
        return False
    print(f"ok    {name}: {len(imports)} imports, all provided by Windows")
    return True


def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <binary> [binary ...]", file=sys.stderr)
        return 2
    # Check everything before deciding, so one run reports every problem.
    if all([check(path) for path in sys.argv[1:]]):
        return 0
    print("\nThis package would fail to start without those DLLs beside it.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
