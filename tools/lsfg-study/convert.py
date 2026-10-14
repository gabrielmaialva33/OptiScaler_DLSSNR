#!/usr/bin/env python3
"""SPIR-V (from lsfg-vk.dll) -> HLSL (spirv-cross) -> DXIL (dxc), for D3D12.

Modes:
  1. Direct DLL / auto-discovery to cache layout (recommended):
       convert.py [path/to/lsfg-vk.dll] [--cache-dir <dir>]
     Auto-discovers lsfg-vk.dll if omitted, hashes the DLL (SHA-256), extracts all 99
     SPIR-V modules from PE RT_RCDATA, and writes compiled .cso directly into:
       <cache-dir>/<hash>/<rid>.cso

  2. Legacy directory conversion:
       convert.py <spv dir> <out dir>

  --fp16-native (study only, mode 1): also converts every module that declares OpCapability Float16 (lsfg-vk's FP16
  variants, ids 51..98) with native 16-bit types: spirv-cross --shader-model 62 --hlsl-enable-16bit-types, dxc
  cs_6_2 -enable-16bit-types. Written to <target>/fp16-native/ (<rid>.cso, hlsl/, manifest.json), beside and never in
  place of the default output, which stays byte-identical. Nothing in OptiScaler reads it: under vkd3d-proton it
  renders byte-identical frames to the default SM 6.0 min16float modules (FP16_NOTES.md, section 7). Needs the release
  dxc.exe under Wine; the Arch /usr/sbin/dxc is an assertion build and aborts on these modules.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

REG = re.compile(r"register\(([btsu])(\d+), space0\)")

SPIRV_OP_CAPABILITY = 17
SPIRV_CAPABILITY_FLOAT16 = 9
FP16_NATIVE_DIR = "fp16-native"
FP16_SPIRV_CROSS = ["--hlsl", "--shader-model", "62", "--hlsl-enable-16bit-types"]
FP16_DXC = ["-T", "cs_6_2", "-enable-16bit-types", "-E", "main", "-O3"]


def remap(hlsl: str):
    bindings = {}

    def sub(m):
        cls, binding = m.group(1), int(m.group(2))
        bindings.setdefault(binding, cls)
        return f"register({cls}0, space{binding})"

    return REG.sub(sub, hlsl), bindings


def declares_float16(spv: bytes) -> bool:
    """True when the module declares OpCapability Float16. Capabilities open a module (SPIR-V 2.4, logical layout),
    so the scan stops at the first other instruction."""
    if len(spv) < 24 or len(spv) % 4:
        return False
    words = struct.unpack_from(f"<{len(spv) // 4}I", spv)
    i = 5  # header: magic, version, generator, bound, schema
    while i < len(words):
        count, opcode = words[i] >> 16, words[i] & 0xFFFF
        if count == 0 or opcode != SPIRV_OP_CAPABILITY:
            return False
        if count >= 2 and words[i + 1] == SPIRV_CAPABILITY_FLOAT16:
            return True
        i += count
    return False


def find_default_dll() -> Path | None:
    """Auto-discover lsfg-vk.dll across standard Steam locations."""
    steam_subpaths = [
        ".local/share/Steam/steamapps/common",
        ".steam/steam/steamapps/common",
        ".steam/debian-installation/steamapps/common",
        ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common",
        "snap/steam/common/.local/share/Steam/steamapps/common",
    ]

    home = os.environ.get("HOME")
    if home:
        home_path = Path(home)
        for rel in steam_subpaths:
            cand = home_path / rel / "Lossless Scaling" / "lsfg-vk.dll"
            if cand.is_file():
                return cand

    xdg = os.environ.get("XDG_DATA_HOME")
    if xdg:
        xdg_path = Path(xdg)
        for rel in steam_subpaths:
            cand = xdg_path / rel / "Lossless Scaling" / "lsfg-vk.dll"
            if cand.is_file():
                return cand

    current = Path("lsfg-vk.dll")
    if current.is_file():
        return current

    return None


def extract_spv_from_pe(data: bytes) -> dict[int, bytes]:
    """Extract RT_RCDATA SPIR-V modules (IDs 0..98) directly from PE bytes."""
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    pe_sig = data[e_lfanew : e_lfanew + 4]
    if pe_sig != b"PE\x00\x00":
        raise ValueError("Invalid PE signature")

    num_sections = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    opt_hdr_size = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    opt_magic = struct.unpack_from("<H", data, e_lfanew + 24)[0]

    # DataDirectory[2] = Resource Directory
    rsrc_dir_offset = e_lfanew + 24 + (112 if opt_magic == 0x20B else 96) + 16
    rsrc_rva, _ = struct.unpack_from("<II", data, rsrc_dir_offset)

    sect_offset = e_lfanew + 24 + opt_hdr_size
    sections = []
    for i in range(num_sections):
        _, s_vsize, s_va, s_rawsize, s_rawptr = struct.unpack_from(
            "<8sIIII", data, sect_offset + i * 40
        )
        sections.append((s_va, max(s_vsize, s_rawsize), s_rawptr))

    def rva_to_offset(rva):
        for s_va, s_size, s_ptr in sections:
            if s_va <= rva < s_va + s_size:
                return rva - s_va + s_ptr
        return None

    rsrc_base = rva_to_offset(rsrc_rva)
    if rsrc_base is None:
        raise ValueError("Cannot map resource section RVA")

    def parse_entries(dir_offset):
        named, id_entries = struct.unpack_from("<HH", data, dir_offset + 12)
        entries = []
        for i in range(named + id_entries):
            name_id, offset = struct.unpack_from(
                "<II", data, dir_offset + 16 + i * 8
            )
            entries.append((name_id, offset))
        return entries

    root_entries = parse_entries(rsrc_base)
    rcdata_matches = [
        e for e in root_entries if (e[0] & 0x80000000 == 0) and e[0] == 10
    ]
    if not rcdata_matches:
        raise ValueError("No RT_RCDATA directory found in PE")

    rcdata_dir = rsrc_base + (rcdata_matches[0][1] & 0x7FFFFFFF)
    spvs = {}

    for rid_raw, rid_offset in parse_entries(rcdata_dir):
        rid = rid_raw
        if rid_raw & 0x80000000:
            # Named entry: e.g. "0" for mipmaps
            str_offset = rsrc_base + (rid_raw & 0x7FFFFFFF)
            str_len = struct.unpack_from("<H", data, str_offset)[0]
            str_val = data[
                str_offset + 2 : str_offset + 2 + str_len * 2
            ].decode("utf-16le", errors="ignore")
            try:
                rid = int(str_val)
            except ValueError:
                rid = 0

        lang_dir = rsrc_base + (rid_offset & 0x7FFFFFFF)
        lang_entries = parse_entries(lang_dir)
        if not lang_entries:
            continue
        data_leaf = rsrc_base + (lang_entries[0][1] & 0x7FFFFFFF)
        data_rva, data_sz = struct.unpack_from("<II", data, data_leaf)
        file_off = rva_to_offset(data_rva)
        if file_off is None:
            continue
        payload = data[file_off : file_off + data_sz]
        if len(payload) >= 4 and struct.unpack_from("<I", payload, 0)[0] == 0x07230203:
            spvs[rid] = payload

    return spvs


def convert_modules(
    modules: dict[int, bytes],
    out_dir: Path,
    repo_root: Path,
    use_wine: bool = True,
):
    """Convert SPIR-V modules to HLSL and DXIL CSO files."""
    hlsl_dir = out_dir / "hlsl"
    dxil_dir = out_dir / "dxil"
    hlsl_dir.mkdir(parents=True, exist_ok=True)
    dxil_dir.mkdir(parents=True, exist_ok=True)

    dxc_wine = repo_root / "OptiScaler/shaders/shader_tools/dxc.exe"
    dxc_native = Path("/usr/sbin/dxc")
    env = dict(
        os.environ,
        WINEPREFIX=os.path.expanduser("~/.local/opt/msvc-wineprefix"),
        WINEDEBUG="-all",
    )

    ok, failed, layout = 0, [], {}

    for rid in sorted(modules.keys()):
        spv_bytes = modules[rid]
        # Run spirv-cross
        p = subprocess.Popen(
            ["spirv-cross", "--hlsl", "--shader-model", "60", "-"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        stdout, stderr = p.communicate(input=spv_bytes.decode("latin1"))
        if p.returncode != 0:
            # Fallback to temp file if stdin fails
            tmp_spv = out_dir / f"tmp_{rid}.spv"
            tmp_spv.write_bytes(spv_bytes)
            stdout = subprocess.run(
                ["spirv-cross", "--hlsl", "--shader-model", "60", str(tmp_spv)],
                capture_output=True,
                text=True,
                check=True,
            ).stdout
            tmp_spv.unlink(missing_ok=True)

        hlsl, bindings = remap(stdout)
        hlsl_file = hlsl_dir / f"{rid}.hlsl"
        hlsl_file.write_text(hlsl)
        cso_file = dxil_dir / f"{rid}.cso"
        direct_cso = out_dir / f"{rid}.cso"

        layout[str(rid)] = {str(b): c for b, c in sorted(bindings.items())}

        if use_wine and dxc_wine.is_file():
            cmd = [
                "wine",
                str(dxc_wine),
                "-T",
                "cs_6_0",
                "-E",
                "main",
                "-O3",
                f"Z:{hlsl_file}",
                "-Fo",
                f"Z:{cso_file}",
            ]
            res = subprocess.run(cmd, capture_output=True, text=True, env=env)
        elif dxc_native.is_file():
            cmd = [
                str(dxc_native),
                "-T",
                "cs_6_0",
                "-E",
                "main",
                "-O3",
                str(hlsl_file),
                "-Fo",
                str(cso_file),
            ]
            res = subprocess.run(cmd, capture_output=True, text=True)
        else:
            failed.append((str(rid), "dxc compiler not found"))
            continue

        if res.returncode == 0 and cso_file.is_file() and cso_file.stat().st_size > 0:
            # Also write directly to out_dir/<rid>.cso for cache layout
            direct_cso.write_bytes(cso_file.read_bytes())
            ok += 1
        else:
            err = "\n".join(
                line
                for line in (res.stdout + res.stderr).splitlines()
                if "MESA" not in line and "pci id" not in line
            )
            failed.append((str(rid), err.strip()[:300]))

    (out_dir / "layout.json").write_text(json.dumps(layout, indent=1))
    print(f"dxc ok={ok} failed={len(failed)}")
    for rid_fail, err_str in failed:
        print(f"FAIL {rid_fail}: {err_str}")


def convert_fp16_native(modules: dict[int, bytes], out_dir: Path, repo_root: Path, dll_hash: str | None = None):
    """Study: native 16-bit conversions of the Float16 modules into <out_dir>/fp16-native/. Returns the failures."""
    target = out_dir / FP16_NATIVE_DIR
    hlsl_dir = target / "hlsl"
    hlsl_dir.mkdir(parents=True, exist_ok=True)

    dxc_wine = repo_root / "OptiScaler/shaders/shader_tools/dxc.exe"
    if not dxc_wine.is_file():
        return [("all", f"{dxc_wine} not found (the native /usr/sbin/dxc asserts on 16-bit modules)")]
    env = dict(
        os.environ,
        WINEPREFIX=os.path.expanduser("~/.local/opt/msvc-wineprefix"),
        WINEDEBUG="-all",
    )

    rids = sorted(rid for rid, spv in modules.items() if declares_float16(spv))
    compiled, failed = [], []
    for rid in rids:
        spv_file = target / f"tmp_{rid}.spv"
        spv_file.write_bytes(modules[rid])
        cross = subprocess.run(["spirv-cross", *FP16_SPIRV_CROSS, str(spv_file)], capture_output=True, text=True)
        spv_file.unlink(missing_ok=True)
        if cross.returncode != 0:
            failed.append((str(rid), cross.stderr.strip()[:300]))
            continue

        hlsl, _ = remap(cross.stdout)
        hlsl_file = hlsl_dir / f"{rid}.hlsl"
        hlsl_file.write_text(hlsl)
        cso_file = target / f"{rid}.cso"
        cso_file.unlink(missing_ok=True)
        res = subprocess.run(
            ["wine", str(dxc_wine), *FP16_DXC, f"Z:{hlsl_file}", "-Fo", f"Z:{cso_file}"],
            capture_output=True,
            text=True,
            env=env,
        )
        if res.returncode == 0 and cso_file.is_file() and cso_file.stat().st_size > 0:
            compiled.append(rid)
        else:
            err = "\n".join(
                line for line in (res.stdout + res.stderr).splitlines() if "MESA" not in line and "pci id" not in line
            )
            failed.append((str(rid), err.strip()[:300]))

    version = subprocess.run(["wine", str(dxc_wine), "--version"], capture_output=True, text=True, env=env)
    dxc_version = next((line.strip() for line in version.stdout.splitlines() if "dxcompiler" in line), "unknown")
    manifest = {
        "kind": "lsfg-fp16-native-study",
        "lsfg_vk_sha256": dll_hash,
        "spirv_cross": FP16_SPIRV_CROSS,
        "dxc": FP16_DXC,
        "dxc_version": dxc_version,
        "rids": compiled,
        "failed": [rid for rid, _ in failed],
    }
    (target / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    print(f"fp16-native ok={len(compiled)} failed={len(failed)} -> {target}")
    for rid_fail, err_str in failed:
        print(f"FAIL fp16-native {rid_fail}: {err_str}")
    return failed


def main():
    repo = Path(__file__).resolve().parents[2]

    # Check for legacy 2-argument invocation: convert.py <spv_dir> <out_dir>
    if len(sys.argv) == 3 and Path(sys.argv[1]).is_dir():
        spv_dir = Path(sys.argv[1])
        out = Path(sys.argv[2])
        modules = {
            int(p.stem): p.read_bytes()
            for p in spv_dir.glob("*.spv")
            if p.stem.isdigit()
        }
        convert_modules(modules, out, repo)
        return

    parser = argparse.ArgumentParser(
        description="Convert LSFG shaders from lsfg-vk.dll directly to hash-keyed cache layout"
    )
    parser.add_argument(
        "dll",
        nargs="?",
        default=None,
        type=Path,
        help="Path to lsfg-vk.dll (auto-discovered if omitted)",
    )
    parser.add_argument(
        "--cache-dir",
        type=Path,
        default=None,
        help="Base cache directory (default: <repo>/lsfg-cache/)",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="Direct output directory override",
    )
    parser.add_argument(
        "--fp16-native",
        action="store_true",
        help="study: also write native 16-bit (cs_6_2) conversions of the FP16 modules to <target>/fp16-native/",
    )
    args = parser.parse_args()

    dll_path = args.dll
    if dll_path is None or not dll_path.is_file():
        dll_path = find_default_dll()

    if dll_path is None or not dll_path.is_file():
        print("Error: Could not locate lsfg-vk.dll. Please specify path explicitly.", file=sys.stderr)
        sys.exit(1)

    print(f"Reading: {dll_path}")
    dll_bytes = dll_path.read_bytes()
    dll_hash = hashlib.sha256(dll_bytes).hexdigest()
    print(f"DLL SHA-256: {dll_hash}")

    if args.out:
        target_dir = args.out
    else:
        base_cache = args.cache_dir if args.cache_dir else (repo / "lsfg-cache")
        target_dir = base_cache / dll_hash

    print(f"Target cache directory: {target_dir}")
    target_dir.mkdir(parents=True, exist_ok=True)

    modules = extract_spv_from_pe(dll_bytes)
    print(f"Extracted {len(modules)} SPIR-V modules from PE resources")
    if len(modules) != 99:
        print(f"Warning: Expected 99 modules, found {len(modules)}", file=sys.stderr)

    convert_modules(modules, target_dir, repo)
    if args.fp16_native:
        convert_fp16_native(modules, target_dir, repo, dll_hash)
    print(f"LSFG shader cache ready at: {target_dir}")


if __name__ == "__main__":
    main()
